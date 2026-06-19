/*
 * ThumbyCue — billiard physics implementation. See cue_physics.h for the
 * model overview. All float; the M33 FPU makes this cheap and there are
 * only ≤22 balls so pairwise work is trivial.
 */
#include "cue_physics.h"
#include <math.h>
#include <string.h>

/* Fixed substep. 2 kHz keeps a fast break (≈6 m/s) to ~3 mm of travel per
 * step — well under a ball radius, so overlap-based collision resolution
 * never tunnels. */
#define CUE_H        (1.0f / 2000.0f)
#define CUE_MAX_SUB  400      /* cap iterations per call (anti death-spiral) */

/* Below these the corresponding motion is treated as stopped. */
#define V_STOP   0.005f       /* m/s linear */
#define U_ROLL   0.01f        /* m/s contact-point slip => rolling */
#define W_STOP   0.05f        /* rad/s spin */

void cue_world_defaults(CueWorld *w, float R, float mass) {
    memset(w, 0, sizeof(*w));
    w->R = R;
    w->mass = mass;
    w->g = 9.806f;
    w->mu_s = 0.20f;          /* ball–cloth sliding (Marlow-ish) */
    w->mu_r = 0.010f;         /* rolling resistance */
    /* Vertical-spin decay: alpha = 5 mu_sp g / (2R). mu_sp ~ 0.022 gives a
     * couple of seconds of carry, which matches real side-spin persistence. */
    w->spin_decel = 5.0f * 0.022f * w->g / (2.0f * R);
    w->e_bb = 0.96f;
    w->mu_bb = 0.06f;         /* ball–ball throw friction */
    w->e_cush = 0.96f;     /* livelier cushions: absorb less energy */
    w->mu_cush = 0.12f;    /* rail friction — full on the roll axis (proper damping + bend) */
    w->cush_spin = 0.30f;  /* but only 30% on the VERTICAL axis → far less side-spin pickup */
    /* Contact point ~0.15 R above centre ⇒ normal tilts up by asin(0.15). Kept
     * modest so top/back spin still bends the rebound a little, but the roll→side
     * coupling that built up running english off the rail is much smaller. */
    w->cush_tilt = asinf(0.15f);
    w->first_hit = -1;
    w->first_hit_idx = -1;
    w->_acc = 0.0f;
}

void cue_phys_strike_elev(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                          float tip_side, float tip_vert, float elev) {
    dir.y = 0.0f;
    dir = v3_norm(dir);
    Vec3 fwd = dir;
    Vec3 up  = v3(0, 1, 0);
    Vec3 right = v3_norm(v3_cross(up, fwd));   /* points to the shooter's right of the aim */

    /* The cue is elevated `elev` rad above horizontal (butt raised → striking
     * DOWN on the ball). The impulse runs along the cue: forward·cos − up·sin.
     * Only the horizontal part drives the ball across the cloth (it can't go
     * down — planar), so travel speed scales with cos(elev). The impulse is
     * applied at the tip contact point r, and the DOWN component acting at a
     * SIDE offset produces spin about the travel axis — which the cloth friction
     * then turns into a curving path (swerve / masse). */
    float ce = cosf(elev), se = sinf(elev);
    Vec3 cdir = v3(fwd.x * ce, -se, fwd.z * ce);     /* cue direction, 3-D */
    b->vel = v3_scale(fwd, speed * ce);
    b->vel.y = 0.0f;

    Vec3 r = v3_add(v3_scale(right, tip_side * w->R),
                    v3_scale(up,    tip_vert * w->R));
    Vec3 J = v3_scale(cdir, speed * w->mass);        /* impulse along the cue */
    float I = 0.4f * w->mass * w->R * w->R;
    b->w = v3_scale(v3_cross(r, J), 1.0f / I);
}

void cue_phys_strike(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                     float tip_side, float tip_vert) {
    cue_phys_strike_elev(w, b, dir, speed, tip_side, tip_vert, 0.0f);
}

/* loudest cushion-approach (normal) speed seen during the current cue_phys_step,
 * so the cushion SFX scales with the actual rail impact, not the whole table. */
static float s_cush_vn;

/* ---- per-ball cloth-contact evolution for one substep ------------------ */
static void ball_cloth(const CueWorld *w, CueBall *b, float h) {
    const float R = w->R, g = w->g;
    Vec3 rc = v3(0, -R, 0);                    /* centre -> contact point */
    /* Contact-point velocity (slip of the ball on the cloth). */
    Vec3 u = v3_add(b->vel, v3_cross(b->w, rc));
    u.y = 0.0f;
    float uh = sqrtf(u.x * u.x + u.z * u.z);

    /* The contact-point slip |u| decays under kinetic friction at (7/2)·μ_s·g
     * (the combined linear + angular effect for a uniform sphere). One substep
     * can therefore kill up to du_full of slip. */
    float du_full = 3.5f * w->mu_s * g * h;
    float I = 0.4f * w->mass * R * R;

    if (uh > du_full) {
        /* SLIDING: full kinetic friction opposing the slip. */
        Vec3 uhat = v3_scale(u, 1.0f / uh);
        Vec3 a = v3_scale(uhat, -w->mu_s * g);
        b->vel = v3_add(b->vel, v3_scale(a, h));
        Vec3 F = v3_scale(a, w->mass);                    /* tau = rc × F */
        b->w = v3_add(b->w, v3_scale(v3_cross(rc, F), h / I));
    } else {
        /* Reaching rolling THIS step: apply exactly enough friction to zero
         * the remaining slip (energy-exact, no snap bump), then roll. */
        if (uh > 1e-6f) {
            Vec3 uhat = v3_scale(u, 1.0f / uh);
            float f = uh / du_full;                       /* < 1: scaled */
            Vec3 a = v3_scale(uhat, -w->mu_s * g * f);
            b->vel = v3_add(b->vel, v3_scale(a, h));
            Vec3 F = v3_scale(a, w->mass);
            b->w = v3_add(b->w, v3_scale(v3_cross(rc, F), h / I));
        }
        /* ROLLING: light resistance; w tracks the (now decreasing) velocity so
         * the slip stays zero. u = 0 ⇒ w.x = vel.z/R, w.z = −vel.x/R. */
        float sp = sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
        if (sp > V_STOP) {
            Vec3 vhat = v3_scale(b->vel, 1.0f / sp);
            b->vel = v3_add(b->vel, v3_scale(vhat, -w->mu_r * g * h));
            if (v3_dot(b->vel, vhat) < 0.0f) b->vel = v3(0, 0, 0);
        } else {
            b->vel = v3(0, 0, 0);
        }
        b->w.x = b->vel.z / R;
        b->w.z = -b->vel.x / R;
    }

    /* Vertical spin (english) decays independently of motion. */
    if (b->w.y > W_STOP)       b->w.y -= w->spin_decel * h;
    else if (b->w.y < -W_STOP) b->w.y += w->spin_decel * h;
    else                       b->w.y = 0.0f;

    b->vel.y = 0.0f;
}

/* Integrate the render orientation from the angular velocity. */
static void ball_spin_orient(CueBall *b, float h) {
    float wl = v3_len(b->w);
    if (wl > 1e-5f) {
        Vec3 axis = v3_scale(b->w, 1.0f / wl);
        m3_rotate_world(&b->orient, axis, wl * h);
    }
}

/* ---- ball–ball impulse (restitution + Coulomb throw) ------------------- */
static int collide_ball_ball(const CueWorld *w, CueBall *bi, CueBall *bj) {
    Vec3 d = v3_sub(bj->pos, bi->pos);
    d.y = 0.0f;
    float dist = sqrtf(d.x * d.x + d.z * d.z);
    float mind = 2.0f * w->R;
    if (dist >= mind || dist < 1e-6f) return 0;

    Vec3 n = v3_scale(d, 1.0f / dist);         /* i -> j */
    /* Separate the overlap so they never stick. */
    float overlap = mind - dist;
    Vec3 push = v3_scale(n, overlap * 0.5f);
    bi->pos = v3_sub(bi->pos, push);
    bj->pos = v3_add(bj->pos, push);

    Vec3 dv = v3_sub(bj->vel, bi->vel);
    float vn = v3_dot(dv, n);
    if (vn >= 0.0f) return 0;                  /* separating already */

    float m = w->mass;
    /* Normal impulse (equal masses, reduced mass m/2). */
    float Jn = -(1.0f + w->e_bb) * vn / (2.0f / m);
    Vec3 Jn_v = v3_scale(n, Jn);
    bi->vel = v3_sub(bi->vel, v3_scale(Jn_v, 1.0f / m));
    bj->vel = v3_add(bj->vel, v3_scale(Jn_v, 1.0f / m));

    /* Tangential friction → throw / spin transfer. Relative surface velocity
     * at the contact point (midway): contact offset is +R*n on i, −R*n on j. */
    Vec3 ri = v3_scale(n,  w->R);
    Vec3 rj = v3_scale(n, -w->R);
    Vec3 si = v3_add(bi->vel, v3_cross(bi->w, ri));
    Vec3 sj = v3_add(bj->vel, v3_cross(bj->w, rj));
    Vec3 s = v3_sub(sj, si);
    Vec3 st = v3_sub(s, v3_scale(n, v3_dot(s, n)));   /* tangential slip */
    float stl = v3_len(st);
    if (stl > 1e-5f) {
        Vec3 that = v3_scale(st, 1.0f / stl);
        /* Tangential effective inverse-mass for two equal spheres at contact:
         * each ball 1/m + R^2/I = 7/(2m); two balls ⇒ 7/m. */
        float Jt_stop = stl / (7.0f / m);
        float Jt_max = w->mu_bb * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        float I = 0.4f * m * w->R * w->R;
        /* j gets +Jt_v, i gets −Jt_v (friction opposes j's slip relative to i). */
        bj->vel = v3_add(bj->vel, v3_scale(Jt_v, 1.0f / m));
        bi->vel = v3_sub(bi->vel, v3_scale(Jt_v, 1.0f / m));
        bj->w = v3_add(bj->w, v3_scale(v3_cross(rj, Jt_v), 1.0f / I));
        bi->w = v3_sub(bi->w, v3_scale(v3_cross(ri, Jt_v), 1.0f / I));
    }
    bi->vel.y = bj->vel.y = 0.0f;
    return 1;
}

/* ---- ball vs an immovable surface with contact normal N (unit, into ball)
 * raised by an optional tilt. Used for cushions (tilted) and jaw circles
 * (horizontal). Returns 1 if a collision was resolved. ------------------- */
static int collide_surface(const CueWorld *w, CueBall *b, Vec3 N,
                           float e, float mu) {
    /* Contact point on the ball is opposite N: r = −R N. The normal impulse
     * is therefore central (no torque); english/throw come from friction. */
    Vec3 r = v3_scale(N, -w->R);
    Vec3 vc = v3_add(b->vel, v3_cross(b->w, r));
    float vn = v3_dot(vc, N);
    if (vn >= 0.0f) return 0;                  /* moving away from the surface */

    float m = w->mass, I = 0.4f * m * w->R * w->R;
    float Jn = -(1.0f + e) * vn * m;           /* central: inverse mass = 1/m */
    Vec3 Jn_v = v3_scale(N, Jn);
    b->vel = v3_add(b->vel, v3_scale(Jn_v, 1.0f / m));

    /* Friction (and thus speed loss / english) only on a genuine impact — a
     * ball merely rolling ALONG the rail has a near-zero approach speed and
     * must not be braked every substep (that was the "sticking"). */
    if (-vn < 0.025f) { b->vel.y = 0.0f; return 1; }

    /* Tangential friction (rail/jaw): opposes the tangential surface slip,
     * which includes side spin — this is english-off-the-cushion. */
    vc = v3_add(b->vel, v3_cross(b->w, r));
    Vec3 vt = v3_sub(vc, v3_scale(N, v3_dot(vc, N)));
    float vtl = v3_len(vt);
    if (vtl > 1e-5f) {
        Vec3 that = v3_scale(vt, -1.0f / vtl);
        float Jt_stop = vtl / (7.0f / (2.0f * m));   /* 1/m + R^2/I = 7/(2m) */
        float Jt_max = mu * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        b->vel = v3_add(b->vel, v3_scale(Jt_v, 1.0f / m));   /* full: bends the bounce */
        /* Apply the angular impulse, but scale ONLY the vertical (side-spin, y)
         * axis by cush_spin: the rail imparts much less NEW english (which just
         * makes the ball texture tumble) while the horizontal (roll) axis keeps
         * full friction so roll is correctly damped — no over-spin build-up, and
         * incoming english still bent the bounce above (vt fed Jt). */
        Vec3 dw = v3_scale(v3_cross(r, Jt_v), 1.0f / I);
        dw.y *= w->cush_spin;
        b->w = v3_add(b->w, dw);
    }
    b->vel.y = 0.0f;
    return 1;
}

/* Closest point on segment [a,b] to point p (X–Z plane). */
static Vec3 seg_closest(Vec3 a, Vec3 b, Vec3 p) {
    Vec3 ab = v3_sub(b, a); ab.y = 0;
    Vec3 ap = v3_sub(p, a); ap.y = 0;
    float L2 = ab.x * ab.x + ab.z * ab.z;
    float t = (L2 > 1e-9f) ? (ap.x * ab.x + ap.z * ab.z) / L2 : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return v3(a.x + ab.x * t, p.y, a.z + ab.z * t);
}

static int collide_cushions(const CueWorld *w, CueBall *b, uint32_t *ev) {
    int hit = 0;
    /* Tilt the rail normal up by cush_tilt so top/back spin couples into the
     * rebound; then re-normalise. */
    float ct = cosf(w->cush_tilt), st = sinf(w->cush_tilt);
    /* Treat the whole cushion chain as a POLYLINE and collide against the single
     * NEAREST contact point. The bounce normal is the smooth vertex-INTERPOLATED
     * normal at the contact (lerp(na,nb,t)) — a continuous normal field along the
     * chain, so the ball sees a single shared normal across the rail↔facing
     * junction instead of bouncing off the kink. Resolved once per step. */
    int best = -1; float best_pen = -1.0f; Vec3 best_n = {0,0,0}, best_sep = {0,0,0};
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *seg = &w->seg[s];
        Vec3 ab = v3_sub(seg->b, seg->a); ab.y = 0.0f;
        Vec3 ap = v3_sub(b->pos, seg->a); ap.y = 0.0f;
        float L2 = ab.x*ab.x + ab.z*ab.z;
        float t = (L2 > 1e-9f) ? (ap.x*ab.x + ap.z*ab.z) / L2 : 0.0f;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        Vec3 cp = v3(seg->a.x + ab.x*t, b->pos.y, seg->a.z + ab.z*t);
        Vec3 d = v3_sub(b->pos, cp); d.y = 0.0f;
        float dist = sqrtf(d.x*d.x + d.z*d.z);
        if (dist >= w->R || dist < 1e-6f) continue;
        Vec3 nd = v3_scale(d, 1.0f / dist);
        if (nd.x*seg->n.x + nd.z*seg->n.z < 0.0f) continue;       /* behind face */
        /* smooth surface normal interpolated between the segment's vertex normals */
        Vec3 sn = v3_norm(v3_add(v3_scale(seg->na, 1.0f - t), v3_scale(seg->nb, t)));
        float pen = w->R - dist;
        if (pen > best_pen) { best_pen = pen; best = s; best_n = sn; best_sep = nd; }
    }
    if (best >= 0) {
        b->pos = v3_add(b->pos, v3_scale(best_sep, best_pen));   /* push out along separation */
        Vec3 N = v3_norm(v3(best_n.x * ct, st, best_n.z * ct));  /* bounce off smooth normal */
        float vn = -(b->vel.x * N.x + b->vel.z * N.z);           /* approach speed into rail */
        if (collide_surface(w, b, N, w->e_cush, w->mu_cush)) {
            hit = 1;
            if (ev) *ev |= CUE_EV_CUSHION;
            if (vn > s_cush_vn) s_cush_vn = vn;                  /* loudest rail impact this step */
        }
    }
    /* Jaw tip circles (immovable) — rattle in the pocket mouths. */
    for (int j = 0; j < w->njaw; j++) {
        Vec3 d = v3_sub(b->pos, w->jaw[j]); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        float mind = w->R + w->jaw_r;
        if (dist < mind && dist > 1e-6f) {
            Vec3 N = v3_scale(d, 1.0f / dist);
            b->pos = v3_add(b->pos, v3_scale(N, (mind - dist)));
            if (collide_surface(w, b, N, w->e_cush, w->mu_cush)) {
                hit = 1;
                if (ev) *ev |= CUE_EV_JAW;
            }
        }
    }
    return hit;
}

static int check_pockets(const CueWorld *w, CueBall *b) {
    for (int p = 0; p < w->npocket; p++) {
        Vec3 d = v3_sub(b->pos, w->pocket[p]); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        if (dist < w->pocket_r[p]) {
            /* begin the drop animation: the ball stays rendered (on=1) and
             * falls into the pocket over ~0.4 s before it's removed. */
            b->pocket = (uint8_t)p;
            b->vel = v3(0, 0, 0);
            b->w = v3(0, 0, 0);
            b->drop = 0.40f;
            return 1;
        }
    }
    return 0;
}

static void substep(CueWorld *w, CueBall *balls, int n, float h, uint32_t *ev) {
    /* 1. cloth friction + integrate. Balls mid-drop instead fall into the
     * pocket (pulled to the centre + accelerating downward) and are removed
     * when they sink below the recess. */
    for (int i = 0; i < n; i++) {
        CueBall *b = &balls[i];
        if (!b->on) continue;
        if (b->drop > 0.0f) {
            Vec3 pc = w->pocket[b->pocket];
            /* Pull the sinking ball further back INTO the pocket (past the mouth).
             * The pocket centre sits radially outward from the table centre, so
             * the radial direction is "deeper in" for every pocket — and for a
             * middle pocket (x≈0 or z≈0) that radial IS the straight-back rail
             * normal. Corners use the deep fall, middles a shallow setback. */
            float back = (b->pocket < 4) ? w->drop_back : w->drop_back_side;
            if (back > 0.0f) {
                float l = sqrtf(pc.x*pc.x + pc.z*pc.z);
                if (l > 1e-5f) { pc.x += pc.x/l * back; pc.z += pc.z/l * back; }
            }
            float k = h * 12.0f; if (k > 1.0f) k = 1.0f;
            b->pos.x += (pc.x - b->pos.x) * k;
            b->pos.z += (pc.z - b->pos.z) * k;
            b->vel.y -= w->g * 0.7f * h;             /* accelerate the fall */
            b->pos.y += b->vel.y * h;
            b->drop -= h;
            if (b->drop <= 0.0f || b->pos.y < -0.11f) { b->on = 0; b->drop = 0.0f; }
            ball_spin_orient(b, h);                  /* keep spinning as it drops */
            continue;
        }
        /* Asleep ball: at rest with no live spin → skip cloth/integrate entirely.
         * A collision in step 2 wakes it (sets velocity). This is exact (a still
         * ball doesn't move) and is the big win on this low-drag cloth, where a
         * shot's long roll-out leaves most balls stopped for ~100+ substeps each. */
        float v2 = b->vel.x*b->vel.x + b->vel.z*b->vel.z;
        if (v2 < V_STOP*V_STOP &&
            b->w.y > -W_STOP && b->w.y < W_STOP &&
            b->w.x > -0.05f && b->w.x < 0.05f &&
            b->w.z > -0.05f && b->w.z < 0.05f) continue;
        ball_cloth(w, b, h);
        b->pos = v3_add(b->pos, v3_scale(b->vel, h));
        b->pos.y = w->R;
        ball_spin_orient(b, h);
    }
    /* 2. ball–ball (skip droppers). Record the CUE ball's (index 0) first
     * object-ball contact for the rules. A cheap per-axis broad-phase reject
     * (exactly equivalent to the dist>=2R early-out, but no sqrt) skips the far
     * pairs — a big win on snooker's 22 balls (O(n^2) pairs per substep). */
    const float bb_min = 2.0f * w->R;
    for (int i = 0; i < n; i++) {
        if (!balls[i].on || balls[i].drop > 0.0f) continue;
        for (int j = i + 1; j < n; j++) {
            if (!balls[j].on || balls[j].drop > 0.0f) continue;
            float dx = balls[i].pos.x - balls[j].pos.x;
            float dz = balls[i].pos.z - balls[j].pos.z;
            if (dx > bb_min || dx < -bb_min || dz > bb_min || dz < -bb_min) continue;
            if (collide_ball_ball(w, &balls[i], &balls[j])) {
                if (ev) *ev |= CUE_EV_BALL_HIT;
                if (w->first_hit < 0 && i == 0) { w->first_hit = balls[j].id; w->first_hit_idx = j; }
            }
        }
    }
    /* 3. cushions + jaws, then 4. pockets (skip droppers AND asleep balls — a
     * resting ball can't be entering a cushion or pocket, and this loop's
     * per-ball cushion/jaw scan is the hot path during the long roll-out). */
    for (int i = 0; i < n; i++) {
        CueBall *b = &balls[i];
        if (!b->on || b->drop > 0.0f) continue;
        float v2 = b->vel.x*b->vel.x + b->vel.z*b->vel.z;
        if (v2 < V_STOP*V_STOP) continue;
        collide_cushions(w, b, ev);
        if (check_pockets(w, b) && ev) *ev |= CUE_EV_POCKET;
    }
}

int cue_phys_moving(const CueWorld *w, const CueBall *balls, int n) {
    for (int i = 0; i < n; i++) {
        if (!balls[i].on) continue;
        const CueBall *b = &balls[i];
        if (b->drop > 0.0f) return 1;          /* wait for the drop to finish */
        float v2 = b->vel.x * b->vel.x + b->vel.z * b->vel.z;
        if (v2 > V_STOP * V_STOP) return 1;
        /* Spinning in place (english on a stationary ball) still counts. */
        if (fabsf(b->w.y) > W_STOP) return 1;
    }
    return 0;
}

/* Substep size actually used. The live game runs at CUE_H (2 kHz); the AI's
 * headless ranking sims switch to a coarser step (cue_phys_set_substep) for ~2x
 * fewer iterations — collision is overlap-based and still well under a ball
 * radius per step, so the leave estimate is unchanged for shot ranking. */
static float g_sub_h = CUE_H;
void cue_phys_set_substep(float h) { g_sub_h = (h > 0.0f) ? h : CUE_H; }

float cue_phys_cushion_impact(void) { return s_cush_vn; }

int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events) {
    if (events) *events = 0;
    s_cush_vn = 0.0f;                  /* reset the cushion-impact meter for this step */
    float h = g_sub_h;
    w->_acc += dt;
    int iters = 0;
    while (w->_acc >= h && iters < CUE_MAX_SUB) {
        substep(w, balls, n, h, events);
        w->_acc -= h;
        iters++;
    }
    if (iters >= CUE_MAX_SUB) w->_acc = 0.0f;   /* shed backlog */

    /* Hard stop once everything has settled so we don't creep forever. */
    if (!cue_phys_moving(w, balls, n)) {
        for (int i = 0; i < n; i++) {
            if (!balls[i].on) continue;
            balls[i].vel = v3(0, 0, 0);
            balls[i].w.y = 0.0f;
            /* leave w.x/w.z = rolling residual; harmless, zero at next strike */
        }
        return 0;
    }
    return 1;
}
