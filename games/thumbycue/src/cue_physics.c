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

/* Hot physics tree -> SRAM on the Mote device module (like ThumbyCraft's CRAFT_HOT).
 * substep() + its callees run dt*2000 times/frame; on a break that's the framerate.
 * The Mote module's code executes from XIP flash by default, which is markedly slower
 * than the native build's hot path — moving this tree to .ramtext (copied to SRAM at
 * load, ABI v36) is the lever that recovers it. No-op on the host build. */
#if defined(MOTE_MODULE_BUILD)
#  define CUE_HOT __attribute__((section(".ramtext.cue")))
#else
#  define CUE_HOT
#endif

/* Below these the corresponding motion is treated as stopped. */
#define V_STOP   0.005f       /* m/s linear */
#define U_ROLL   0.01f        /* m/s contact-point slip => rolling */
#define W_STOP   0.05f        /* rad/s spin */

void cue_world_defaults(CueWorld *w, float R, float mass) {
    memset(w, 0, sizeof(*w));
    /* A skittle is 15 g of light wood on a 15 mm base and it TOPPLES rather
     * than being driven along, so it takes far less off the ball than its mass
     * would suggest: about a third of the approach speed, which is a clip and a
     * change of line rather than a bounce. Below a crawl it rocks without going
     * over — AEBBA rule 103, which scores and re-spots rather than fouling. */

    w->R = R;
    w->mass = mass;
    w->g = 9.806f;
    /* Two thirds of a ball radius of drop before the far lip of a bed hole
     * stops being a ramp and becomes a wall. Tuned so a bar billiards ball
     * rolled at a hole goes down and one driven at it skips across, which is
     * how the table plays. */
    w->hole_catch = 0.66f;
    w->mu_s = 0.20f;          /* ball–cloth sliding (Marlow-ish) */
    w->mu_r = 0.010f;         /* rolling resistance */
    /* Vertical-spin decay: alpha = 5 mu_sp g / (2R). mu_sp ~ 0.022 gives a
     * couple of seconds of carry, which matches real side-spin persistence. */
    w->spin_decel = 5.0f * 0.022f * w->g / (2.0f * R);
    w->e_bb = 0.96f;
    w->mu_bb = 0.06f;         /* ball–ball throw friction */
    w->e_cush = 0.96f;     /* at a crawl; cue_table sets the real per-table set */
    w->cush_efall = 0.050f;
    w->e_cush_min = 0.55f;
    w->mu_cush = 0.12f;    /* rail friction — full on the roll axis (proper damping + bend) */
    w->cush_spin = 0.30f;  /* but only 30% on the VERTICAL axis → far less side-spin pickup */
    /* Contact point ~0.15 R above centre ⇒ normal tilts up by asin(0.15). Kept
     * modest so top/back spin still bends the rebound a little, but the roll→side
     * coupling that built up running english off the rail is much smaller. */
    w->cush_tilt = asinf(0.15f);
    /* The bed. Cloth over slate returns very little: a jumped ball takes two or
     * three quick diminishing hops and is down. v_land is set so the last hop
     * is under a millimetre — below that it settles flat rather than chatter,
     * because every bounce suspends cloth friction and a ball that never quite
     * lands never develops roll. */
    w->e_bed = 0.40f;
    w->v_land = 0.14f;        /* ~1 mm of rise */
    /* Overwritten by cue_table with the real nose; a sane value until then so a
     * world built by hand does not let balls fly out over zero-height rails. */
    w->cushion_nose = 1.27f * R;
    w->bound_x = w->bound_z = 1.0e9f;   /* until cue_table says where the rail ends */
    w->nplay = w->nbound = 0;           /* ...and what shape it is */
    cue_phys_shot_begin(w);
    w->_acc = 0.0f;
}

/* ---- the shot's own record ------------------------------------------------ */

/* THE STEP THE ENGINE OWNS. Set by the front end — the handheld hands over the
 * ABI's, CueVR and the tests hand over mote_phys_step. Unset, the pins stand
 * still, which is honest and is what a front end that never set it deserves. */
static CuePhysRigidFn s_rigid;
void cue_phys_set_rigid(CuePhysRigidFn fn) { s_rigid = fn; }

/* ---- THE PIN'S COLLISION SHAPE ------------------------------------------
 *
 * A capsule at the stem's radius is the shape a BALL meets — Rule 74 keeps
 * every ball on the plain cylinder — but it is not the shape the CLOTH meets.
 * A mushroom lying down rests on the rim of its cap and the flare of its foot,
 * and a stem-width capsule has neither: the pin lay down with its head buried
 * in the bed, which is exactly what it looked like.
 *
 * So the body is a convex hull: the pin's own silhouette turned about its axis
 * — bottom centre, foot rim, cap rim, top — which fills the waist under the
 * brim. That is the right simplification, because the waist is the one part of
 * a mushroom that never touches anything: it rests on the two rims, which is
 * what the hull's own edge does. The ball is unaffected either way, since
 * check_skittles measures against the stem itself and not against this.
 */
#define SK_HULL_SEG 12
static Vec3    s_hull_v[2 * SK_HULL_SEG + 2];
static Vec3    s_hull_n[3 * SK_HULL_SEG];
static uint8_t s_hull_off[3 * SK_HULL_SEG + 1];
static uint8_t s_hull_fv[SK_HULL_SEG * 10];
static uint8_t s_hull_e[2 * 5 * SK_HULL_SEG];
static MoteHull s_hull;
static int      s_hull_ok;

static void skittle_hull_build(float sr, float len) {
    const int N = SK_HULL_SEG;
    /* the silhouette, as radii of the stem and heights above the foot, taken
     * from the turned profile: the foot's flare, the cap's rim, the top. */
    const float r1 = 1.62f * sr, y1 = 0.000f;
    const float r2 = 3.40f * sr, y2 = 0.097f;
    const float y0 = 0.000f,     y3 = len;
    const float c  = len * 0.5f;            /* the body's origin: the centre */
    int nv = 0;
    s_hull_v[nv++] = v3(0, y0 - c, 0);                       /* 0: the foot   */
    for (int i = 0; i < N; i++) {
        const float a = 6.2831853f * (float)i / N;
        s_hull_v[nv++] = v3(r1 * cosf(a), y1 - c, r1 * sinf(a));
    }
    for (int i = 0; i < N; i++) {
        const float a = 6.2831853f * (float)i / N;
        s_hull_v[nv++] = v3(r2 * cosf(a), y2 - c, r2 * sinf(a));
    }
    const int TOP = nv;
    s_hull_v[nv++] = v3(0, y3 - c, 0);                       /* the cap's top */

    int nf = 0, fo = 0;
    #define FACE_BEGIN() (s_hull_off[nf] = (uint8_t)fo)
    #define FACE_END()   do {         /* outward, judged against the body's own centre */         const uint8_t *fv = &s_hull_fv[s_hull_off[nf]];         Vec3 A = s_hull_v[fv[0]], B = s_hull_v[fv[1]], C = s_hull_v[fv[2]];         Vec3 n = v3_cross(v3_sub(B, A), v3_sub(C, A));         float l = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z); if (l < 1e-9f) l = 1.0f;         n = v3_scale(n, 1.0f / l);         if (n.x*A.x + n.y*A.y + n.z*A.z < 0.0f) n = v3_scale(n, -1.0f);         s_hull_n[nf] = n; nf++;     } while (0)
    for (int i = 0; i < N; i++) {          /* the foot, a fan */
        const int a = 1 + i, b = 1 + (i + 1) % N;
        FACE_BEGIN();
        s_hull_fv[fo++] = 0; s_hull_fv[fo++] = (uint8_t)a; s_hull_fv[fo++] = (uint8_t)b;
        FACE_END();
    }
    for (int i = 0; i < N; i++) {          /* the flank, foot rim to cap rim */
        const int a = 1 + i, b = 1 + (i + 1) % N;
        const int c2 = 1 + N + i, d2 = 1 + N + (i + 1) % N;
        FACE_BEGIN();
        s_hull_fv[fo++] = (uint8_t)a; s_hull_fv[fo++] = (uint8_t)b;
        s_hull_fv[fo++] = (uint8_t)d2; s_hull_fv[fo++] = (uint8_t)c2;
        FACE_END();
    }
    for (int i = 0; i < N; i++) {          /* the dome, a fan to the top */
        const int a = 1 + N + i, b = 1 + N + (i + 1) % N;
        FACE_BEGIN();
        s_hull_fv[fo++] = (uint8_t)a; s_hull_fv[fo++] = (uint8_t)b;
        s_hull_fv[fo++] = (uint8_t)TOP;
        FACE_END();
    }
    s_hull_off[nf] = (uint8_t)fo;
    #undef FACE_BEGIN
    #undef FACE_END

    int ne = 0;
    for (int i = 0; i < N; i++) {
        const int a = 1 + i, b = 1 + (i + 1) % N;
        const int c2 = 1 + N + i, d2 = 1 + N + (i + 1) % N;
        s_hull_e[ne*2+0] = 0;            s_hull_e[ne*2+1] = (uint8_t)a;  ne++;
        s_hull_e[ne*2+0] = (uint8_t)a;   s_hull_e[ne*2+1] = (uint8_t)b;  ne++;
        s_hull_e[ne*2+0] = (uint8_t)a;   s_hull_e[ne*2+1] = (uint8_t)c2; ne++;
        s_hull_e[ne*2+0] = (uint8_t)c2;  s_hull_e[ne*2+1] = (uint8_t)d2; ne++;
        s_hull_e[ne*2+0] = (uint8_t)c2;  s_hull_e[ne*2+1] = (uint8_t)TOP; ne++;
    }

    float br = 0.0f;
    for (int i = 0; i < nv; i++) {
        const float d = sqrtf(s_hull_v[i].x*s_hull_v[i].x +
                              s_hull_v[i].y*s_hull_v[i].y +
                              s_hull_v[i].z*s_hull_v[i].z);
        if (d > br) br = d;
    }
    s_hull.verts = s_hull_v;   s_hull.nverts = nv;
    s_hull.fnorm = s_hull_n;   s_hull.nfaces = nf;
    s_hull.faceoff = s_hull_off; s_hull.facev = s_hull_fv;
    s_hull.edges = s_hull_e;   s_hull.nedges = ne;
    s_hull.bound_r = br;
    s_hull_ok = 1;
}

/* Stand pin k up on its spot, at rest. The body's origin is its CENTRE OF
 * MASS, so a pin standing on the cloth has its centre half its length up. */
static void skittle_stand(CueWorld *w, int k) {
    MoteBody *b = &w->sk[k];
    memset(b, 0, sizeof *b);
    const float L = w->skittle_len, R = w->skittle_r;
    if (s_hull_ok) {
        b->shape      = MOTE_SHAPE_HULL;
        b->shape_data = &s_hull;
        b->radius     = s_hull.bound_r;
    } else {                                  /* no hull: the bare stem */
        b->shape  = MOTE_SHAPE_CAPSULE;
        b->radius = R;
        b->half   = v3(0.0f, (L * 0.5f > R) ? (L * 0.5f - R) : 0.0f, 0.0f);
    }
    b->inv_mass = (w->skittle_mass > 0.0f) ? 1.0f / w->skittle_mass : 0.0f;
    b->pos      = v3(w->skittle[k].x, L * 0.5f, w->skittle[k].z);
    b->vel      = v3(0,0,0);
    b->w        = v3(0,0,0);
    b->orient   = (Mat3){{{1,0,0},{0,1,0},{0,0,1}}};
    /* Wood on cloth: it stops quickly and does not bounce much. */
    b->friction    = 0.62f;
    b->restitution = 0.12f;
}

/* The world the pins fall about in: the bed is its floor and the cushions are
 * its walls, which for bar billiards — the one table with skittles, and a
 * rectangle — is exactly what mote's bounding box already is. */
void cue_phys_skittles_init(CueWorld *w, float half_len, float half_wid) {
    if (!w) return;
    mote_phys_world_defaults(&w->sk_world);
    w->sk_world.gravity = v3(0.0f, -w->g, 0.0f);
    w->sk_world.walls   = 0;                 /* real planes, see below */
    w->sk_world.bmin    = v3(-half_len, 0.0f, -half_wid);
    w->sk_world.bmax    = v3( half_len, 0.60f, half_wid);
    w->sk_world.restitution = 0.12f;
    w->sk_world.friction    = 0.62f;
    /* The same clock the balls keep, so a pin and a ball cannot disagree about
     * when a contact happened, and enough substeps that the cap never bites. */
    w->sk_world.substep      = CUE_H;
    w->sk_world.max_substeps = CUE_MAX_SUB;
    w->sk_world.linear_damp  = 0.20f;
    w->sk_world.angular_damp = 0.35f;
    skittle_hull_build(w->skittle_r, w->skittle_len);
    for (int k = 0; k < w->nskittle; k++) skittle_stand(w, k);
    /* THE BED AND THE FOUR CUSHIONS, as static planes: normal along r[1] and
     * a point on the surface. */
    {   int n = w->nskittle;
        const struct { Vec3 nrm, at; } pl[5] = {
            { v3(0, 1, 0), v3(0, 0, 0) },                       /* the bed */
            { v3(-1, 0, 0), v3( half_len, 0, 0) },              /* the rails */
            { v3( 1, 0, 0), v3(-half_len, 0, 0) },
            { v3(0, 0,-1), v3(0, 0,  half_wid) },
            { v3(0, 0, 1), v3(0, 0, -half_wid) },
        };
        for (int i = 0; i < CUE_SKITTLE_PLANES && n < CUE_SKITTLE_BODIES; i++, n++) {
            MoteBody *b = &w->sk[n];
            memset(b, 0, sizeof *b);
            b->shape    = MOTE_SHAPE_PLANE;
            b->inv_mass = 0.0f;               /* a plane is always immovable */
            b->pos      = pl[i].at;
            b->orient   = (Mat3){{{1,0,0},{0,1,0},{0,0,1}}};
            b->orient.r[1] = pl[i].nrm;       /* the normal, solid below it */
            b->friction    = 0.62f;
            b->restitution = 0.12f;
        }
        w->sk_n = n;
    }
    w->sk_on = 1;
}

void cue_phys_shot_begin(CueWorld *w) {
    if (!w) return;
    w->first_hit = -1;
    w->first_hit_idx = -1;
    w->jump_over = 0; w->jump_over_id = 0;
    w->jmp_pending = 0; w->jmp_idx = -1; w->jmp_hit_it = 0; w->jmp_bounced = 0;
    w->ntouch = 0; w->touch_over = 0;
    w->side_cushion = 0;
    /* WHAT THIS STROKE DID TO THE PINS is a fact about this stroke, so the
     * bookkeeping is cleared here. WHETHER A PIN IS LYING DOWN is not — it is
     * the state of a body on the cloth, and it stays true until somebody
     * stands it up. See cue_phys_skittles_respot, which the host calls once
     * the stroke has been judged. */
    for (int k = 0; k < CUE_MAX_SKITTLE; k++) {
        w->skittle_order[k] = 0;
        w->skittle_nudged[k] = 0;
    }
    w->skittle_fell = 0;
}

/* STAND THEM BACK UP, once the stroke has been judged.
 *
 * This used to happen at the START of the next stroke, which is why the pins
 * appeared to right themselves at the moment the cue struck rather than when
 * the shot was over — the reported oddity. The rules must SEE the fallen pins
 * to price the stroke (Rules 110(f), 111(a), 112), so this cannot run before
 * cue_rules_resolve; the host calls it straight afterwards, which is also when
 * a marker would reach across and do it. */
void cue_phys_skittles_respot(CueWorld *w) {
    if (!w) return;
    for (int k = 0; k < w->nskittle; k++) {
        w->skittle[k] = w->skittle_spot[k];
        if (w->sk_on) skittle_stand(w, k);
        w->skittle_down[k] = 0;
        w->skittle_nudged[k] = 0;
        w->skittle_order[k] = 0;
    }
    w->skittle_fell = 0;
}

/* Append to the cue ball's account. Only ever called for ball 0. */
static void touch_add(CueWorld *w, uint8_t what, uint8_t id, uint8_t idx) {
    if (w->ntouch >= CUE_MAX_TOUCH) { w->touch_over = 1; return; }
    CueTouch *t = &w->touch[w->ntouch++];
    t->what = what; t->id = id; t->idx = idx; t->_pad = 0;
}

int cue_touch_count(const CueWorld *w) { return w ? w->ntouch : 0; }

int cue_touch_get(const CueWorld *w, int i, CueTouch *out) {
    if (!w || !out || i < 0 || i >= w->ntouch) return 0;
    *out = w->touch[i];
    return 1;
}

int cue_touch_cushions(const CueWorld *w) {
    int n = 0;
    if (!w) return 0;
    for (int i = 0; i < w->ntouch; i++)
        if (w->touch[i].what == CUE_TOUCH_CUSHION) n++;
    return n;
}

int cue_touch_cannon(const CueWorld *w, int id_a, int id_b) {
    int a = 0, b = 0;
    if (!w) return 0;
    for (int i = 0; i < w->ntouch; i++) {
        if (w->touch[i].what != CUE_TOUCH_BALL) continue;
        if (w->touch[i].id == (uint8_t)id_a) a = 1;
        if (w->touch[i].id == (uint8_t)id_b) b = 1;
    }
    return a && b;
}

int cue_touch_cushions_before_second_ball(const CueWorld *w) {
    int first = -1, cush = 0;
    if (!w) return -1;
    for (int i = 0; i < w->ntouch; i++) {
        if (w->touch[i].what == CUE_TOUCH_CUSHION) { cush++; continue; }
        if (first < 0) { first = w->touch[i].id; continue; }
        /* A second contact with the SAME ball is not the second ball. The cue
         * ball can come back onto the first one and often does. */
        if (w->touch[i].id != (uint8_t)first) return cush;
    }
    return -1;   /* it never reached a second ball: not zero, and not the same */
}

/* Cue-ball deflection (squirt) at full side — declared in cue_physics.h so a
 * player who MEANS to use side can aim off for it, the way a real one does. */

void cue_phys_strike_jump(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                          float tip_side, float tip_vert, float elev, float vy) {
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

    /* Off centre costs almost no PACE. An earlier version scaled the drive by
     * sqrt(1 - offset^2), reasoning that only the component along the line of
     * centres translates — which is true of the impulse but wrong about the
     * outcome: a cue ball struck near its edge leaves at very nearly the speed
     * of one struck in the middle, it just leaves spinning and pointing
     * slightly elsewhere. Scaling the pace made side and screw feel like a
     * power penalty, and at large offsets it cancelled the shot entirely.
     *
     * What off centre really does, besides the spin below, is SQUIRT: the ball
     * departs a little away from the side the tip struck, because the tip has to
     * shove the ball's mass sideways to get there. It is a couple of degrees at
     * most on a modern shaft, and deliberately kept small here — enough to be
     * felt as a thing to allow for, not enough to make aiming a guess. */
    float squirt = -tip_side * CUE_SQUIRT_RAD;      /* right-hand side -> left */
    float cq = cosf(squirt), sq = sinf(squirt);
    Vec3 aim = v3_norm(v3_add(v3_scale(fwd, cq), v3_scale(right, sq)));
    b->vel = v3_scale(aim, speed * ce);
    /* THE ONE PLACE THE BALL LEAVES THE CLOTH. The downward part of the cue's
     * impulse squeezes the ball against the slate and the bed throws it back;
     * how much survives is the caller's number, for the reasons in the header.
     * Zero here is the planar game exactly as it was. */
    b->vel.y = vy > 0.0f ? vy : 0.0f;

    /* WHERE THE TIP TOUCHES, IN THE CUE'S OWN FRAME.
     *
     * tip_side and tip_vert are measured perpendicular to the CUE — that is how
     * the contact is found, a sphere against a sphere along the cue's axis — so
     * the lever arm has to be built the same way. It was built from the world's
     * up instead, which is only perpendicular to the cue while the cue is flat.
     * Raise the butt and that vector swings towards the cue's own direction,
     * where it contributes nothing to r × J: at 75 degrees a half-ball of
     * backspin produced 26% of the spin it produces flat, and the steeper the
     * cue the less it did. Massé and jump shots were the shots that suffered,
     * which is exactly where a player expects the most spin, not the least.
     *
     * `right` was always correct — it is horizontal and square to the cue at
     * any elevation — so only the vertical axis changes. */
    Vec3 vert = v3_norm(v3_cross(cdir, right));
    /* AND THE TIP CANNOT REACH PAST HALF A BALL.
     *
     * A cue tip on a sphere slips once the contact is much beyond half the
     * radius — the miscue limit, and it is geometry and friction, not a
     * question of chalk. The contact test here is sphere against sphere, so the
     * game happily allowed a strike out at the very edge, where the arithmetic
     * gives omega*R/v = 2.5: exactly double what any cue can do, and reported
     * as balls zipping round the table and spinning like tops. Capped at the
     * limit, so full offset gives 1.25, which is the real maximum. */
    float ts = tip_side, tv = tip_vert;
    {   float m = sqrtf(ts*ts + tv*tv);
        if (m > CUE_TIP_MAX) { float k = CUE_TIP_MAX / m; ts *= k; tv *= k; } }
    const float BR = cue_ball_r(w, b), BM = cue_ball_m(w, b);
    Vec3 r = v3_add(v3_scale(right, ts * BR),
                    v3_scale(vert,  tv * BR));
    Vec3 J = v3_scale(cdir, speed * BM);             /* impulse along the cue */
    float I = 0.4f * BM * BR * BR;
    b->w = v3_scale(v3_cross(r, J), 1.0f / I);
}

void cue_phys_strike_elev(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                          float tip_side, float tip_vert, float elev) {
    cue_phys_strike_jump(w, b, dir, speed, tip_side, tip_vert, elev, 0.0f);
}

void cue_phys_strike(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                     float tip_side, float tip_vert) {
    cue_phys_strike_jump(w, b, dir, speed, tip_side, tip_vert, 0.0f, 0.0f);
}

/* Off the bed by enough to matter. The epsilon is a tenth of a millimetre: a
 * ball resting on the cloth must never test airborne, or it stops feeling
 * friction and rolls for ever. */
int cue_phys_airborne(const CueWorld *w, const CueBall *b) {
    return b->pos.y > cue_ball_r(w, b) + 1.0e-4f;
}

/* loudest cushion-approach (normal) speed seen during the current cue_phys_step,
 * so the cushion SFX scales with the actual rail impact, not the whole table. */
static float s_cush_vn;
/* Set when a ball met the bed during the current step, so the caller can make
 * the noise a jumped ball makes when it comes down. */
static int s_bed_land;

/* ---- per-ball cloth-contact evolution for one substep ------------------ */
static CUE_HOT void ball_cloth(const CueWorld *w, CueBall *b, float h) {
    const float R = cue_ball_r(w, b), g = w->g;
    Vec3 rc = v3(0, -R, 0);                    /* centre -> contact point */
    /* Contact-point velocity (slip of the ball on the cloth). */
    Vec3 u = v3_add(b->vel, v3_cross(b->w, rc));
    u.y = 0.0f;
    float uh = sqrtf(u.x * u.x + u.z * u.z);

    /* The contact-point slip |u| decays under kinetic friction at (7/2)·μ_s·g
     * (the combined linear + angular effect for a uniform sphere). One substep
     * can therefore kill up to du_full of slip. */
    float du_full = 3.5f * w->mu_s * g * h;
    float bm = cue_ball_m(w, b);
    float I = 0.4f * bm * R * R;

    if (uh > du_full) {
        /* SLIDING: full kinetic friction opposing the slip. */
        Vec3 uhat = v3_scale(u, 1.0f / uh);
        Vec3 a = v3_scale(uhat, -w->mu_s * g);
        b->vel = v3_add(b->vel, v3_scale(a, h));
        Vec3 F = v3_scale(a, bm);                    /* tau = rc × F */
        b->w = v3_add(b->w, v3_scale(v3_cross(rc, F), h / I));
    } else {
        /* Reaching rolling THIS step: apply exactly enough friction to zero
         * the remaining slip (energy-exact, no snap bump), then roll. */
        if (uh > 1e-6f) {
            Vec3 uhat = v3_scale(u, 1.0f / uh);
            float f = uh / du_full;                       /* < 1: scaled */
            Vec3 a = v3_scale(uhat, -w->mu_s * g * f);
            b->vel = v3_add(b->vel, v3_scale(a, h));
            Vec3 F = v3_scale(a, bm);
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

    /* A BALL ON THE CLOTH HAS NO VERTICAL MOTION — unless something has just
     * given it some. This flattened it unconditionally, which is correct for
     * the ordinary case and is also why a ball could never leave the cushion:
     * the impact handed it an upward velocity and the next substep of cloth
     * contact threw it away before the airborne test upstairs ever saw it.
     * Rising is now left alone; anything falling is still put flat on the
     * bed, which is what stops a ball jittering on the cloth. */
    if (b->vel.y < 0.0f) b->vel.y = 0.0f;
}

/* Integrate the render orientation from the angular velocity. */
static CUE_HOT void ball_spin_orient(CueBall *b, float h) {
    float wl = v3_len(b->w);
    if (wl > 1e-5f) {
        Vec3 axis = v3_scale(b->w, 1.0f / wl);
        m3_rotate_world(&b->orient, axis, wl * h);
    }
}

/* ---- ball–ball impulse (restitution + Coulomb throw) ------------------- */
static CUE_HOT int collide_ball_ball(const CueWorld *w, CueBall *bi, CueBall *bj) {
    /* HEIGHT COUNTS, but only when one of them is off the bed.
     *
     * The separation test was flat, which is right for a planar game and wrong
     * the moment anything jumps: a ball sailing a clear inch over another would
     * still smash into it. Two balls both on the cloth have identical y, so
     * this is the same test it always was for every shot that does not leave
     * the bed — including the arithmetic, which is why nothing that used to
     * work moves by a bit. */
    const float ri_r = cue_ball_r(w, bi), rj_r = cue_ball_r(w, bj);
    const float sumr = ri_r + rj_r;
    float dy = bj->pos.y - bi->pos.y;
    if (dy > sumr || dy < -sumr) return 0;
    Vec3 d = v3_sub(bj->pos, bi->pos);
    d.y = 0.0f;
    if (dy != 0.0f) {
        /* One is airborne: the flat gap has to shrink by the height between
         * them or they pass through each other's shadow. */
        float flat2 = d.x * d.x + d.z * d.z;
        if (flat2 + dy * dy >= sumr * sumr) return 0;
    }
    float dist = sqrtf(d.x * d.x + d.z * d.z);
    float mind = sumr;
    if (dist >= mind || dist < 1e-6f) return 0;

    Vec3 n = v3_scale(d, 1.0f / dist);         /* i -> j */
    /* Separate the overlap so they never stick. Split by SIZE, so a small cue
     * ball is pushed further than the object ball it met, as the lighter and
     * smaller of the two should be. */
    float overlap = mind - dist;
    Vec3 push = v3_scale(n, overlap * 0.5f);
    bi->pos = v3_sub(bi->pos, push);
    bj->pos = v3_add(bj->pos, push);

    Vec3 dv = v3_sub(bj->vel, bi->vel);
    float vn = v3_dot(dv, n);
    if (vn >= 0.0f) return 0;                  /* separating already */

    /* Normal impulse. Written with both masses because English pool's cue ball
     * is lighter than the balls it strikes — 94 g against 116 — and with equal
     * masses this is exactly the old m/2 reduced mass. */
    const float mi = cue_ball_m(w, bi), mj = cue_ball_m(w, bj);
    float Jn = -(1.0f + w->e_bb) * vn / (1.0f / mi + 1.0f / mj);
    Vec3 Jn_v = v3_scale(n, Jn);
    bi->vel = v3_sub(bi->vel, v3_scale(Jn_v, 1.0f / mi));
    bj->vel = v3_add(bj->vel, v3_scale(Jn_v, 1.0f / mj));

    /* Tangential friction → throw / spin transfer. Relative surface velocity
     * at the contact point (midway): contact offset is +R*n on i, −R*n on j. */
    Vec3 ri = v3_scale(n,  ri_r);
    Vec3 rj = v3_scale(n, -rj_r);
    Vec3 si = v3_add(bi->vel, v3_cross(bi->w, ri));
    Vec3 sj = v3_add(bj->vel, v3_cross(bj->w, rj));
    Vec3 s = v3_sub(sj, si);
    Vec3 st = v3_sub(s, v3_scale(n, v3_dot(s, n)));   /* tangential slip */
    float stl = v3_len(st);
    if (stl > 1e-5f) {
        Vec3 that = v3_scale(st, 1.0f / stl);
        /* Tangential effective inverse-mass at the contact. For one sphere it
         * is 1/m + r^2/I = 7/(2m), whatever its radius, so the pair gives
         * 7/(2mi) + 7/(2mj) — and with equal masses that is the old 7/m. */
        float Ii = 0.4f * mi * ri_r * ri_r;
        float Ij = 0.4f * mj * rj_r * rj_r;
        float inv_t = 3.5f / mi + 3.5f / mj;
        float Jt_stop = stl / inv_t;
        float Jt_max = w->mu_bb * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        /* `that` is the direction j's surface is sliding RELATIVE TO i's, so
         * friction on j is along MINUS it, and i takes the reaction. The four
         * lines here used to be the other way round — the comment above them
         * said "friction opposes j's slip" and the code then applied +Jt_v to j,
         * which is friction along the slip.
         *
         * That inverts throw. Measured out of the engine (test_throw.c), a
         * plain cut with no side threw the object ball up to 4 degrees off the
         * ghost-ball line and in the WRONG DIRECTION — a cut was overcut where
         * every real table undercuts. On a one-metre pot 4 degrees is 68 mm,
         * which is most of a snooker pocket, and it is why cue_ai.h had to claim
         * "the engine pots cleanly, so the ghost-ball aim is the true aim" while
         * the planner quietly missed cuts it rated highly. It also made side
         * unusable: turn it on and every sided shot missed by the throw. */
        bj->vel = v3_sub(bj->vel, v3_scale(Jt_v, 1.0f / mj));
        bi->vel = v3_add(bi->vel, v3_scale(Jt_v, 1.0f / mi));
        bj->w = v3_sub(bj->w, v3_scale(v3_cross(rj, Jt_v), 1.0f / Ij));
        bi->w = v3_add(bi->w, v3_scale(v3_cross(ri, Jt_v), 1.0f / Ii));
    }
    bi->vel.y = bj->vel.y = 0.0f;
    return 1;
}

/* ---- ball vs an immovable surface with contact normal N (unit, into ball)
 * raised by an optional tilt. Used for cushions (tilted) and jaw circles
 * (horizontal). Returns 1 if a collision was resolved. ------------------- */
static CUE_HOT int collide_surface(const CueWorld *w, CueBall *b, Vec3 N,
                           float e, float mu) {
    /* Contact point on the ball is opposite N: r = −R N. The normal impulse
     * is therefore central (no torque); english/throw come from friction. */
    Vec3 r = v3_scale(N, -cue_ball_r(w, b));
    Vec3 vc = v3_add(b->vel, v3_cross(b->w, r));
    float vn = v3_dot(vc, N);
    if (vn >= 0.0f) return 0;                  /* moving away from the surface */

    float m = cue_ball_m(w, b), I = 0.4f * m * cue_ball_r(w, b) * cue_ball_r(w, b);
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

/* ---- ball vs CUSHION: the impact integrated over its own impulse ---------
 *
 * The cushion used to be handled by collide_surface above: one instantaneous
 * Coulomb impulse against a tilted wall. That model is in a class which
 * provably cannot do what a real cushion does. Biber, Champneys & Szalai
 * (arXiv:2208.11685) show that a rigid contact with a Poisson or energetic
 * restitution "cannot capture slip reversal during the contact phase" — the
 * slip direction is frozen at the instant of contact — and that the fix is to
 * let the impact take TIME, during which the slip direction rotates.
 *
 * So this integrates the impact over its own accumulated normal impulse, which
 * is the scheme in Mathavan, Jackson & Parkin, "A theoretical analysis of
 * billiard ball dynamics under cushion impacts" (Proc IMechE Part C, 224:1863,
 * 2010), equations 14a-14f. Their measurements were taken on a Riley
 * Renaissance — the World Snooker table — with snooker balls, so the constants
 * below are this game's own sport rather than a generic sphere:
 *
 *     e_e = 0.98      ball-cushion restitution (energetic, Stronge)
 *     mu_w = 0.14     ball-cushion friction
 *     mu_s = 0.212    ball-cloth friction, which acts DURING the impact
 *     h = 7R/5        contact height, so sin(theta) = 2/5
 *
 * Two contacts act at once — the cushion at I and the cloth at C — and that
 * second one is why the old model could not be fixed by tuning: it was not
 * there at all.
 *
 * AND THE BALL CAN LEAVE THE CLOTH. Their equation 8 gives the cloth's normal
 * impulse as (sin@ + mu_w sin(phi) cos@) per unit of cushion impulse. A cloth
 * cannot pull downwards, so when that goes negative the table has lost the
 * ball and the vertical velocity is free — the ball lifts. That is the real
 * trigger, not a special case bolted on: hit hard enough into the nose above
 * centre and it hops, which is what the cushion's slope exists to limit. The
 * paper notes the same thing about its own rigid assumption at speed.
 */
#ifndef CUE_CUSH_STEPS
#define CUE_CUSH_STEPS 96      /* impulse increments; 96 lands within 0.1% of 512 */
#endif
/* Where the rigid-cushion assumption stops being true, from the paper. */
#ifndef CUE_CUSH_RIGID_V
#define CUE_CUSH_RIGID_V 2.5f
#endif
/* How briskly the ball climbs the deforming nose past that. Not measured —
 * see the note where it is used. 0.10 gives about 7 mm of hop at 5 m/s. */
#ifndef CUE_CUSH_LIFT
#define CUE_CUSH_LIFT 0.10f
#endif
/* Ceiling on the hop, as a fraction of the nose height above ball centre. */
#ifndef CUE_CUSH_HOP
#define CUE_CUSH_HOP 0.55f
#endif

static CUE_HOT int cushion_impact(const CueWorld *w, CueBall *b, Vec3 n_face,
                                  float sin_th)
{
    /* The paper's frame: X along the cushion, Y into it, Z up. */
    Vec3 up = v3(0,1,0);
    Vec3 yh = v3(-n_face.x, 0.0f, -n_face.z);
    float yl = sqrtf(yh.x*yh.x + yh.z*yh.z);
    if (yl < 1e-6f) return 0;
    yh = v3_scale(yh, 1.0f/yl);
    Vec3 xh = v3_norm(v3_cross(yh, up));            /* right-handed with up */

    float vx = b->vel.x*xh.x + b->vel.y*xh.y + b->vel.z*xh.z;
    float vy = b->vel.x*yh.x + b->vel.y*yh.y + b->vel.z*yh.z;
    float vz = b->vel.y;
    if (vy <= 0.0f) return 0;                        /* not approaching */

    float wx = b->w.x*xh.x + b->w.y*xh.y + b->w.z*xh.z;
    float wy = b->w.x*yh.x + b->w.y*yh.y + b->w.z*yh.z;
    float wz = b->w.y;

    const float M = cue_ball_m(w, b), R = cue_ball_r(w, b);
    const float S = sin_th, C = sqrtf(1.0f - S*S);

    /* RESTITUTION FALLS WITH PACE, because the rubber deforms further the
     * harder it is struck and gets less of that back.
     *
     * A constant was measurably the wrong SHAPE, not merely the wrong number:
     * square into the rail this model returned 89-90% of the speed at 2 m/s and
     * the same 89-90% at 7, so a firm shot lost nothing extra and every angle
     * off the rail played the same however hard it was hit. Mathavan's
     * high-speed imaging of a snooker table has the rebound ratio at 0.910 for
     * a ball barely moving and a 0.818 best fit across 0.28-3.5 m/s, i.e.
     * sliding down through normal play, and Marlow's rails come in near 0.55 —
     * which is where a really firm one ends up.
     *
     * Linear in the approach speed, anchored on those two, floored so it cannot
     * go absurd on a break. e_cush is now the LOW-SPEED figure. */
    float e_v = w->e_cush - w->cush_efall * (vy*sqrtf(1.0f - sin_th*sin_th) + vz*sin_th);
    if (e_v < w->e_cush_min) e_v = w->e_cush_min;
    if (e_v > w->e_cush)     e_v = w->e_cush;
    const float e2 = e_v * e_v;
    const float muw = w->mu_cush, mus = w->mu_s;
    /* On the cloth the ball is held; in the air there is no second contact. */
    const int on_cloth = (b->pos.y <= R * 1.02f);

    /* A BALL LEANING ON THE CUSHION IS NOT AN IMPACT. Below a crawl there is
     * no impact to integrate — the ball is resting against the rubber or
     * creeping along it, and running the full scheme there lets it settle a
     * few millimetres the wrong side of the cushion line in a mitred pocket
     * corner, where the segment normals fight each other. The old single
     * impulse had the same guard for the same reason; it just reflects. */
    float zeta_in = vy*C + vz*S;
    if (zeta_in < 0.025f) {
        vy = -vy * e_v;
        b->vel = v3(vx*xh.x + vy*yh.x, 0.0f, vx*xh.z + vy*yh.z);
        return 1;
    }

    /* Total impulse is about (1+e)Mv_normal; take it in equal slices. */
    float Ptot = (1.0f + e_v) * M * zeta_in;
    if (Ptot <= 0.0f) return 0;
    float dP = Ptot / (float)CUE_CUSH_STEPS;

    float W = 0.0f, Wc = 0.0f;
    int compressing = 1;
    float zeta = vy*C + vz*S;                        /* closing speed along Z' */
    const float zeta0 = zeta, wx0 = wx, vy0 = vy;    /* as it arrived */

    for (int n = 0; n < CUE_CUSH_STEPS * 6; n++) {
        /* Slip at the cushion (I) and at the cloth (C). atan2 so the direction
         * is a real angle and free to swing round during the contact — which is
         * the whole point of integrating rather than resolving in one go. */
        float sI_n = -vy*S + vz*C + wx*R;
        float sI_d =  vx + wy*R*S - wz*R*C;
        float phi  = atan2f(sI_n, sI_d);
        float sC_n =  vy + wx*R;
        float sC_d =  vx - wy*R;
        float phiC = atan2f(sC_n, sC_d);
        int   slipI = (sI_n*sI_n + sI_d*sI_d) > 1e-8f;
        int   slipC = on_cloth && (sC_n*sC_n + sC_d*sC_d) > 1e-8f;

        float cph = slipI ? cosf(phi)  : 0.0f, sph = slipI ? sinf(phi)  : 0.0f;
        float cpc = slipC ? cosf(phiC) : 0.0f, spc = slipC ? sinf(phiC) : 0.0f;
        float mw  = slipI ? muw : 0.0f;
        float ms  = slipC ? mus : 0.0f;

        /* The cloth's share of the impulse (their eq. 8). Negative means the
         * cloth would have to PULL the ball down to keep it there. */
        float kC = S + mw*sph*C;
        float kCu = on_cloth ? (kC > 0.0f ? kC : 0.0f) : 0.0f;

        float dvx = -(dP/M) * (mw*cph + ms*cpc*kCu);
        float dvy = -(dP/M) * (C - mw*S*sph + ms*spc*kCu);
        float dvz =  (dP/M) * (kCu - kC);            /* zero while the cloth holds */
        float g   = 5.0f*dP/(2.0f*M*R);
        float dwx = -g * (mw*sph + ms*spc*kCu);
        float dwy = -g * (mw*cph*S - ms*cpc*kCu);
        float dwz =  g * (mw*cph*C);

        vx += dvx; vy += dvy; vz += dvz;
        wx += dwx; wy += dwy; wz += dwz;

        float z2 = vy*C + vz*S;
        W += dP * 0.5f * (zeta + z2);
        zeta = z2;
        if (compressing && zeta <= 0.0f) { compressing = 0; Wc = W; }
        if (!compressing && W <= (1.0f - e2) * Wc) break;
        if (!compressing && zeta < 0.0f && W <= 0.0f) break;
    }

    /* ---- AND OFF THE CLOTH, WHEN IT IS HIT HARD ENOUGH --------------------
     *
     * Everything above is inside the rigid-cushion assumption, and inside it
     * the ball can never leave the table: the cloth's share of the impulse is
     * sin(theta) + mu_w sin(phi) cos(theta), which cannot go negative for any
     * slip angle at this geometry, so there is nothing to lift it.
     *
     * That assumption has a stated limit. Mathavan et al. put it at 2.5 m/s of
     * NORMAL velocity, and say why: past it the cushion stops behaving like a
     * rigid body, the nose is driven back and up, and "the normal ball velocity
     * at I ... will try to lift up the tip of the cushion". Their own words for
     * the cushion's slope are that it exists "in order to prevent the ball from
     * leaping up in the air after impact" — a precaution against something that
     * evidently happens.
     *
     * So above that speed the ball climbs. This is EXTRAPOLATION, not
     * measurement: nobody has published outgoing vertical velocities off a
     * cushion, and this is a plain ramp in the excess over the rigid limit,
     * scaled by how much the ball is rolling INTO the nose, which is the
     * direction of climb. It is deliberately small — a hard shot hops a few
     * millimetres and lands, which is what it looks like across a table. The
     * engine already handles a ball in flight properly (it keeps its spin,
     * which is measured behaviour), so the landing takes care of itself. */
    float over = zeta0 - CUE_CUSH_RIGID_V;
    if (over > 0.0f) {
        float roll_in = -(wx0 * R) / (fabsf(vy0) + 1e-3f);   /* + is into the nose */
        if (roll_in < 0.0f) roll_in = 0.0f;
        if (roll_in > 2.0f) roll_in = 2.0f;
        float lift = CUE_CUSH_LIFT * over * (0.5f + 0.5f * roll_in);
        /* AND NOT OVER THE NOSE. The slope of a cushion exists to stop the
         * ball climbing it, so a hop that carries the ball above the contact
         * line is not a hop, it is the ball getting on top of the rail — and
         * from up there it rolls off the outside of the table, which is a lost
         * ball rather than a lively cushion. Cap the climb at a fraction of
         * the nose's height above ball centre: the ball leaves the cloth, and
         * lands again in front of the cushion where it belongs. Measured: at
         * the uncapped value, four shots in a thousand ended up resting the
         * wrong side of the cushion line. */
        float nose = R * S;                       /* nose height above centre */
        float vmax = sqrtf(2.0f * w->g * CUE_CUSH_HOP * (nose > 0.0f ? nose : 0.0f));
        if (lift > vmax) lift = vmax;
        vz += lift;
    }

    b->vel = v3(vx*xh.x + vy*yh.x, vz, vx*xh.z + vy*yh.z);
    b->w   = v3(wx*xh.x + wy*yh.x, wz, wx*xh.z + wy*yh.z);
    if (b->vel.y < 0.0f) b->vel.y = 0.0f;            /* never driven downward */
    return 1;
}

/* Closest point on segment [a,b] to point p (X–Z plane). */
static CUE_HOT Vec3 seg_closest(Vec3 a, Vec3 b, Vec3 p) {
    Vec3 ab = v3_sub(b, a); ab.y = 0;
    Vec3 ap = v3_sub(p, a); ap.y = 0;
    float L2 = ab.x * ab.x + ab.z * ab.z;
    float t = (L2 > 1e-9f) ? (ap.x * ab.x + ap.z * ab.z) / L2 : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return v3(a.x + ab.x * t, p.y, a.z + ab.z * t);
}

static CUE_HOT int collide_cushions(const CueWorld *w, CueBall *b, uint32_t *ev) {
    int hit = 0;
    /* OVER THE CUSHION. A ball whose underside has cleared the TOP of the
     * cushion is past the rail, not bouncing off it — that is what a jump shot
     * out of a snooker is. Everything on the cloth fails this test by a mile,
     * so the planar game is untouched.
     *
     * The top, which is rail_top. `cushion_nose` is a different height and an
     * easy one to reach for by name: it is the ball-CONTACT line, 63.5% of a
     * ball up the front face, where a rolling ball touches. The cushion carries
     * on above it to the flat top the frame is level with (cue_render: nose_h,
     * then flat_h = nose_h * 1.30, and rail_h = flat_h). Judging "cleared it"
     * at the contact line let a ball pass through 10 mm of solid drawn cushion,
     * which is the reported ball-through-the-rail, and it also opened a band
     * with no wall and no floor in it. */
    if (b->pos.y - cue_ball_r(w, b) > w->rail_top) return 0;
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
        if (dist >= cue_ball_r(w, b) || dist < 1e-6f) continue;
        Vec3 nd = v3_scale(d, 1.0f / dist);
        if (nd.x*seg->n.x + nd.z*seg->n.z < 0.0f) continue;       /* behind face */
        /* smooth surface normal interpolated between the segment's vertex normals */
        Vec3 sn = v3_norm(v3_add(v3_scale(seg->na, 1.0f - t), v3_scale(seg->nb, t)));
        float pen = cue_ball_r(w, b) - dist;
        if (pen > best_pen) { best_pen = pen; best = s; best_n = sn; best_sep = nd; }
    }
    if (best >= 0) {
        b->pos = v3_add(b->pos, v3_scale(best_sep, best_pen));   /* push out along separation */
        Vec3 N = v3_norm(v3(best_n.x * ct, st, best_n.z * ct));  /* bounce off smooth normal */
        float vn = -(b->vel.x * N.x + b->vel.z * N.z);           /* approach speed into rail */
        /* WHERE ON THE BALL THE CUSHION ACTUALLY TOUCHES, which depends on how
         * high the ball is — not on a constant taken from the table's spec.
         *
         * The nose sits at a fixed height above the cloth. A ball at rest meets
         * it above its own centre, which is the whole point of the design: the
         * impulse has a downward component and the ball is held on the table.
         * A ball that is ALREADY IN THE AIR meets it lower down, and once the
         * contact is below the centre the same impulse points UP — which is a
         * ball climbing the cushion, and is how a jumped ball gets over a rail
         * rather than rebounding off it.
         *
         * It also settles what a hard topspin shot can do from the cloth: it
         * cannot climb, because the impact reverses the horizontal motion
         * first, so whatever lift it gets happens while it is already leaving.
         * That falls out of the geometry rather than needing a rule. */
        float nose_y = cue_ball_r(w, b) * st;                     /* nose above cloth-rest centre */
        float rise   = b->pos.y - cue_ball_r(w, b);               /* how high the ball is */
        float sn     = (nose_y - rise) / cue_ball_r(w, b);        /* + above centre, - below */
        if (sn >  0.95f) sn =  0.95f;
        if (sn < -0.95f) sn = -0.95f;
        if (cushion_impact(w, b, best_n, sn)) {
            hit = 1;
            /* Rule 108 asks whether the last-ball shot went off a SIDE
             * cushion, and only the impact knows which cushion it was: a
             * side's normal points across the table, an end's points along
             * it. Recorded for the shot, read by the rules at the settle. */
            if (ev && (best_n.z > 0.7f || best_n.z < -0.7f))
                *ev |= CUE_EV_SIDE_CUSH;
            if (ev) *ev |= CUE_EV_CUSHION;
            if (vn > s_cush_vn) s_cush_vn = vn;                  /* loudest rail impact this step */
        }
    }
    /* Jaw tip circles (immovable) — rattle in the pocket mouths. */
    for (int j = 0; j < w->njaw; j++) {
        Vec3 d = v3_sub(b->pos, w->jaw[j]); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        float mind = cue_ball_r(w, b) + w->jaw_r;
        if (dist < mind && dist > 1e-6f) {
            if (b->pos.y - cue_ball_r(w, b) > w->rail_top) continue;       /* flying over it */
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

/* THE POCKET: A MOUTH BETWEEN THE JAWS, AND A BACK BEHIND IT.
 *
 * It used to be a radius test — cross the line and the ball was captured on the
 * spot, its velocity discarded and an animation played. Two things followed.
 * A ball had to be ON THE CLOTH to be potted, because an airborne one had
 * nowhere to go, so a tenth of a millimetre of hop was enough for a pocket to
 * refuse a ball rolling straight into it. And nothing about the shot survived
 * the crossing: pace, angle and spin stopped mattering at the line.
 *
 * A pocket is not a circle that swallows. It is an OPENING BOUNDED BY THE TWO
 * JAWS, with solid material everywhere else, and its back is shaped to do a
 * job: gather the ball in and take the pace off it. So the question is which
 * side of the jaw-to-jaw line the ball is on, not how far it is from a centre.
 * Past that line it is in the throat, and the only way out is back through the
 * mouth — which is exactly what a rattle is.
 *
 * A radius could not express that. Behind the mouth the cut is wider than any
 * circle centred on the pocket, which is how a ball at 8 m/s crossed the
 * opening, left outside the radius and ended up rolling down the outside of the
 * table: the containment stopped where the circle did.
 *
 * In the throat the back does what a real one does — pace off, ball gathered
 * toward the middle — and then it falls, because there is no slate under it.
 * Nothing is scripted, and nothing has to ask whether the ball is in the air.
 */
#define CUE_POCKET_FLOOR (-0.10f)   /* the floor of the recess (m) */
/* How hard the lip gathers a falling ball toward the pocket's axis, per second
 * squared per metre off it. Sized so a ball crossing at the rim is drawn to the
 * middle within the time it takes to fall — which is what the roll of the cloth
 * does to it on a real table. */
/* Softened from 250. That was sized to drag a ball crossing at the rim into the
 * middle within the time it takes to fall, which it did — and at a middle
 * pocket, where the catch sits deeper, it was picking the ball up and throwing
 * it at the far wall. It only has to STEER the ball, not fling it: the ball is
 * already falling, and the lip below still carries it down. */
#ifndef CUE_LIP_GATHER
#define CUE_LIP_GATHER 90.0f
#endif

/* HOW FAR PAST THE EDGE OF THE SLATE a point is, at pocket p. Negative is still
 * on cloth, zero is the edge, positive is out over the drop.
 *
 * The cut is an arc around `cut_c` with a straight tangent leg out to each slate
 * edge — a quarter circle at a corner, a half at a middle. That makes the
 * distance to it exact and cheap: which of the three pieces is nearest falls out
 * of the sign of the point's two local coordinates, so there is no search. The
 * renderer walks the identical curve. */
CUE_HOT float cue_phys_cut_out(const CueWorld *w, int p, float x, float z) {
    Vec3  C = w->cut_c[p];
    float R = w->cut_r[p];
    /* A hole in the open bed has no legs and no rail to run them out to: the
     * missing cloth is the circle, and only the circle. */
    if (w->pocket_bed[p]) {
        float u = x - C.x, v = z - C.z;
        return R - sqrtf(u*u + v*v);
    }
    float sz = (C.z < 0.0f) ? -1.0f : 1.0f;
    float v  = sz * (z - C.z);
    if (p >= 4) {                                   /* a middle: half arc, two legs */
        float u = x - C.x;
        if (v > 0.0f) return R - (u < 0.0f ? -u : u);        /* between the legs */
        return R - sqrtf(u*u + v*v);                         /* on the arc */
    }
    float sx = (C.x < 0.0f) ? -1.0f : 1.0f;         /* a corner: quarter arc, two legs */
    float u  = sx * (x - C.x);
    if (u <= 0.0f && v <= 0.0f) return R - sqrtf(u*u + v*v); /* facing the arc */
    if (v > 0.0f && u > 0.0f)                                /* deep in the cut */
        return (u < v ? u : v) + R;
    return (v > 0.0f ? u : v) + R;                           /* against one leg */
}

/* THE SKITTLES GO OVER, they do not bounce a ball back.
 *
 * A bar billiards skittle is eleven centimetres of light wood standing on a
 * fifteen-millimetre base. A ball that reaches one knocks it flat and carries
 * on; modelling it as a post to rebound from would be a worse lie about the
 * table than ignoring the deflection is. So this only records the fall — and
 * records the ORDER, because Rule 112 prices a white and a black differently
 * depending on which of them went first.
 *
 * A ball in the air over a skittle does not touch it: they are 11 cm tall and
 * a jumped ball can clear one. Returns 1 the moment a skittle goes down. */
static CUE_HOT int check_skittles(CueWorld *w, CueBall *b) {
    if (!w->nskittle) return 0;
    const float R = cue_ball_r(w, b);
    /* Over the top of it. A skittle is 114 mm of light wood; a ball whose
     * bottom is above that has cleared it. */
    if (b->pos.y - R > 0.114f) return 0;
    int fell = 0;
    for (int k = 0; k < w->nskittle; k++) {
        /* A FALLEN SKITTLE IS STILL AN OBJECT. It used to be skipped once it
         * was down — the ball rolled straight over a pin lying on the cloth,
         * which was defensible when "down" meant an animation had finished and
         * is nonsense now that it means a body is lying there. It is hit like
         * anything else; the capsule test below runs along the pin's axis and
         * so works lying down exactly as it does standing up. */
        MoteBody *sb = &w->sk[k];

        /* THE NEAREST POINT ON THE PIN, which is what a capsule is: the ball
         * meets the STEM (Rule 74 keeps every ball below 51 mm and a ball's
         * highest point is under that), and where on the stem decides whether
         * the pin is knocked over or spun on its foot. A pin lying down is
         * met along its length, which is why this is a segment and not a
         * point. */
        const Vec3 ax = sb->orient.r[1];                 /* the pin's own axis */
        /* THE STEM, from the pin's own dimensions — NOT from the body's half
         * extent. The body is a hull (so that the cap rests on the cloth) and
         * a hull has no half.y, so reading it gave zero: the contact point
         * collapsed onto the centre of mass, the lever arm vanished, and a
         * struck pin was shoved across the table without turning a degree. */
        float hl2 = 0.5f * w->skittle_len - w->skittle_r;
        if (hl2 < 0.0f) hl2 = 0.0f;
        Vec3 d0 = v3_sub(b->pos, sb->pos);
        float t = d0.x*ax.x + d0.y*ax.y + d0.z*ax.z;
        if (t >  hl2) t =  hl2;
        if (t < -hl2) t = -hl2;
        const Vec3 cp = v3_add(sb->pos, v3_scale(ax, t));   /* on the axis */
        Vec3 dv = v3_sub(b->pos, cp);
        const float reach = R + w->skittle_r;
        float q = dv.x*dv.x + dv.y*dv.y + dv.z*dv.z;
        if (q > reach * reach) continue;

        float d = sqrtf(q);
        Vec3 n;
        if (d > 1e-6f) n = v3_scale(dv, 1.0f / d);
        else           n = v3(1,0,0);                    /* dead centre: pick one */

        /* THE COLLISION, BOTH WAYS. The ball is a moving sphere and the pin is
         * a rigid body, so this is the ordinary impulse between the two — and
         * because the contact is well below the pin's centre of mass, the same
         * impulse that slows the ball also spins the pin, which is what makes
         * it topple rather than slide away. Nothing here has to decide that it
         * falls: the arithmetic does. */
        const Vec3 rc = v3_sub(cp, sb->pos);             /* body arm */
        const Vec3 vp = v3_add(sb->vel, v3_cross(sb->w, rc));
        const Vec3 rel = v3_sub(b->vel, vp);
        const float vn = rel.x*n.x + rel.y*n.y + rel.z*n.z;
        const float speed = sqrtf(b->vel.x*b->vel.x + b->vel.z*b->vel.z);

        (void)speed;
        if (vn >= 0.0f) continue;                        /* already separating */

        /* THE SAME INERTIA THE SOLVER USES. mote models a hull's as a sphere of
         * its bounding radius, and a contact resolved here against a different
         * tensor from the one that integrates it afterwards is two solvers
         * disagreeing about the same pin. One number, taken from mote's own
         * rule, so they cannot. */
        const float m = (sb->inv_mass > 0.0f) ? 1.0f / sb->inv_mass : 0.012f;
        float br = sb->radius;
        if (sb->shape == MOTE_SHAPE_HULL && sb->shape_data)
            br = ((const MoteHull *)sb->shape_data)->bound_r;
        const float Ieff = (br > 1e-6f) ? (m * br * br / 2.5f) : (m * 1e-4f);
        const Vec3 rn = v3_cross(rc, n);
        const float ang = (rn.x*rn.x + rn.y*rn.y + rn.z*rn.z) / Ieff;
        const float mb  = cue_ball_m(w, b);
        const float denom = 1.0f/mb + sb->inv_mass + ang;
        if (denom < 1e-9f) continue;
        const float e = 0.15f;                           /* wood takes little back */
        const float j = -(1.0f + e) * vn / denom;

        b->vel = v3_add(b->vel, v3_scale(n, j / mb));
        sb->vel = v3_sub(sb->vel, v3_scale(n, j * sb->inv_mass));
        /* dw = (r x -j n) / I — isotropic, so no change of basis is needed. */
        sb->w = v3_add(sb->w, v3_scale(v3_cross(rc, v3_scale(n, -j)), 1.0f / Ieff));
        sb->_reserved[0] = 0;                            /* wake it */
        fell = 1;                                        /* something was struck */
    }
    return fell;
}

/* ---- A HOLE CUT IN THE OPEN BED ------------------------------------------
 *
 * Bar billiards, and nothing else here. A hole barely wider than the ball,
 * with cloth all the way round it, is not a pocket: there is no mouth, no
 * jaws, no throat, and no rail behind it to stop anything. What decides
 * whether the ball goes down is FREE FALL.
 *
 * The ball rolls on the cloth until its centre crosses the near lip; from
 * there to the far lip it is over nothing and falls under gravity. It is
 * unsupported for the chord its centre cuts across the hole, which takes
 *
 *      t = chord / v      and it falls    y = g t^2 / 2
 *
 * At the far lip the lip meets the ball at bed level. If the ball's centre is
 * still above the bed, the lip is BELOW the ball's equator: it pushes up and
 * onward and the ball rims out, which is what a ball driven at a hole does.
 * If the ball has fallen far enough, the lip is above the equator and pushes
 * it down — it is caught.
 *
 * The whole character of the game falls out of that one line: you can pot the
 * 200 from anywhere on the table, but only at a pace that gives it time to
 * drop, and thumping it just skips it across the hole.
 *
 * Returns 1 = down, 0 = it survived (and has been kicked off the far lip). */
static CUE_HOT int bed_hole(const CueWorld *w, CueBall *b, int p) {
    const float R = cue_ball_r(w, b);
    const float a = w->pocket_r[p];
    const float cx = w->drop_c[p].x, cz = w->drop_c[p].z;
    float vx = b->vel.x, vz = b->vel.z;
    float v = sqrtf(vx*vx + vz*vz);
    if (v < 1e-4f) return 1;                 /* stopped over it: it drops in */

    /* The chord the CENTRE cuts: how far it is unsupported. The perpendicular
     * distance from the hole's centre to the path decides it. */
    const float ux = vx / v, uz = vz / v;
    const float dx = cx - b->pos.x, dz = cz - b->pos.z;
    float perp = dx * uz - dz * ux;          /* signed miss distance */
    float pa = perp < 0.0f ? -perp : perp;
    if (pa >= a) return 0;                   /* the centre clips the lip, no more */
    const float half = sqrtf(a*a - pa*pa);

    /* THE WHOLE CHORD, not the part that is left.
     *
     * The question is how far the ball has fallen by the time it reaches the
     * FAR lip, and it has been unsupported since its centre crossed the near
     * one — so the answer is about the full chord and nothing else. Taking
     * the remaining distance from wherever the step happened to land the ball
     * made it depend on the substep: the live game samples at 2 kHz and the
     * AI ranks shots at 600 Hz, so the AI would have judged the same shot
     * harder than it is and stopped playing pots that go in. */
    const float span = 2.0f * half;

    const float t = span / v;
    const float fall = 0.5f * w->g * t * t;
    const float need = w->hole_catch * R;
    if (fall >= need) return 1;              /* below the lip: caught */

    /* RIMMED OUT. The far lip is under the ball's equator, so it takes the
     * ball up and away from the hole — hard and clean when it crossed fast
     * and barely dropped, a rattling half-turn when it nearly went down. */
    const float f = (need > 1e-6f) ? (fall / need) : 0.0f;   /* 0 fast .. 1 close */
    /* Where it leaves: the far end of the chord, measured from the hole. */
    const float along = dx * ux + dz * uz;   /* + means the hole is ahead */
    float ex = b->pos.x + ux * (along + half) - cx;
    float ez = b->pos.z + uz * (along + half) - cz;
    float el = sqrtf(ex*ex + ez*ez);
    if (el > 1e-6f) { ex /= el; ez /= el; } else { ex = ux; ez = uz; }
    const float vn = vx * ex + vz * ez;
    if (vn < 0.0f) {                          /* still heading into the lip */
        const float e = 0.15f + 0.55f * f;    /* the slower it crossed, the more
                                               * of its pace the lip takes back */
        b->vel.x -= (1.0f + e) * vn * ex;
        b->vel.z -= (1.0f + e) * vn * ez;
    }
    /* ...and it comes off the lip climbing, which is the little hop you see. */
    b->vel.x *= (1.0f - 0.20f * f);
    b->vel.z *= (1.0f - 0.20f * f);
    b->vel.y += 0.35f * (0.3f + 0.7f * f) * v;
    if (b->pos.y < R) b->pos.y = R;
    return 0;
}

static CUE_HOT int check_pockets(const CueWorld *w, CueBall *b) {
    if (b->pos.y - cue_ball_r(w, b) > w->rail_top) return 0;         /* flying clean over */
    for (int p = 0; p < w->npocket; p++) {
        Vec3 d = v3_sub(b->pos, w->drop_c[p]); d.y = 0.0f;
        float q = d.x*d.x + d.z*d.z;
        float reach = w->cut_r[p] + cue_ball_r(w, b) * 3.0f;
        if (q > reach * reach) {
            /* Well clear of it: whatever this hole decided about the ball is
             * spent, and it may be judged afresh if it comes back. Clearing
             * this only on the near miss below left a ball that had rimmed
             * out and rolled away permanently immune to the hole it escaped. */
            if (b->over_hole == (uint8_t)(p + 1)) b->over_hole = 0;
            continue;
        }

        /* THE DROP IS THE POCKET CIRCLE. The ball's centre inside it and the
         * ball is down — which is a circle about the pocket, so the cut drawn
         * round the pocket can be fitted to it exactly, and what you watch drop
         * is what the physics did. */
        int in = (q <= w->pocket_r[p] * w->pocket_r[p]);

        /* AND NOTHING ELSE. There was a second test — which side of the line
         * between the two jaw tips the ball was on — put there because a circle
         * alone once leaked: behind the mouth the opening is wider than any
         * circle centred on the pocket, so a ball arriving hard could cross the
         * throat outside the radius the whole way and come out the far side
         * still travelling, with correctly no slate back there to stop it.
         * Twelve balls per thousand on the 9 ft table.
         *
         * But a straight line has no ends. Combined with the proximity test it
         * captured a HALF DISC, and at a corner a ball hugging the rail crosses
         * that line well to the side of the mouth — pot area in FRONT of the
         * pocket, which is the one place there must be none.
         *
         * It is also no longer earning it. The leak does not reproduce: 1044
         * shots per table at every pocket, speed and angle, and 16,632 more
         * fired hard from twenty angles, all lose nothing with the circle
         * alone. The balls that used to escape now rattle off the jaws and come
         * back out, which is what a jaw is for. Whatever closed it — the
         * cushion model, the frame surface, both — the circle is sound on its
         * own, and a circle is the one shape the cut drawn round the pocket can
         * be fitted to exactly.
         *
         * AND ONE WAS FOUND AGAIN, the moment the drop was tuned: pushing the
         * US table's catch 0.28 R deeper put it behind where a hard ball
         * crosses the throat, and four went off the back of it. So the second
         * test is the one this comment already said it should be — A BALL WITH
         * NO CLOTH UNDER IT IS POTTED. That region is the cut in the slate and
         * nothing else: it cannot reach onto the table, because on the table
         * there is cloth. It only ever fires for a ball the circle missed,
         * which is a ball already through the opening, and now that the cut and
         * the drop have been tuned onto each other it is a fraction of a
         * millimetre of daylight rather than a rule of its own. */
        if (!in && cue_phys_cut_out(w, p, b->pos.x, b->pos.z) > 0.0f) in = 1;
        if (!in) { if (b->over_hole == (uint8_t)(p + 1)) b->over_hole = 0; continue; }

        /* A HOLE IN THE BED IS JUDGED ONCE, ON THE WAY IN. The free-fall sum
         * is about the whole crossing, so asking it again every step while
         * the ball is still inside the circle would swallow a ball that has
         * already been thrown out. */
        if (w->pocket_bed[p]) {
            if (b->over_hole == (uint8_t)(p + 1)) return 0;   /* already judged */
            b->over_hole = (uint8_t)(p + 1);
            if (!bed_hole(w, b, p)) return 0;
        }

        if (b->drop <= 0.0f) {
            b->pocket = (uint8_t)p;
            b->drop = 1.0f;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* IS (x, z) ON THE CLOTH?
 *
 * A rectangle and an L are a union of rectangles and answer in two compares. A
 * regular bed is not a union of rectangles at all, so it carries its own
 * outline and is answered by half-planes — which is exact, and cheap, because
 * every shape in that family is convex.
 *
 * One function so that no caller has to know which kind of table it is on. */
int cue_world_on_bed(const CueWorld *w, float x, float z) {
    if (w->nbedv >= 3) {
        for (int i = 0; i < w->nbedv; i++) {
            int j = (i + 1 == w->nbedv) ? 0 : i + 1;
            float ex = w->bedv_x[j] - w->bedv_x[i];
            float ez = w->bedv_z[j] - w->bedv_z[i];
            /* the outward normal of a run is (ez, -ex); inside is behind it */
            if ((x - w->bedv_x[i]) * ez - (z - w->bedv_z[i]) * ex > 0.0f)
                return 0;
        }
        return 1;
    }
    return w->nplay ? cue_rects_contain(w->play_r, w->nplay, x, z) : 1;
}

/* WHAT IS UNDER A BALL AT (x, z): the cloth, the top of the frame, or nothing.
 *
 * Three regions and a hole. Inside the cushion line is the bed, MINUS the bite
 * each pocket takes out of it. Outside it, cushion top and wood top are ONE
 * FLAT LEVEL at rail_top — that is how cue_render builds them (rail_h =
 * flat_h), so anything else here would be a floor that is not where the table
 * is drawn. Past the frame there is a floor this does not model, so the ball
 * has left. And over a pocket there is no top at all — which is how a ball
 * rattling along the frame can drop in, and it does happen.
 *
 * NOT quite the answer cue_table_surface gives the cue, and that is on purpose.
 * That one is asked how high the butt must be to clear the table, and near a
 * pocket it still reports solid cloth: erring toward a surface that is MORE
 * solid than the real one can only ask the player to lift the cue a fraction
 * more, which is safe, where erring the other way drops the shaft through the
 * slate. This one decides whether a ball is held up, and there the truth
 * matters in both directions. */
#define CUE_NO_SURFACE (-1.0e9f)
static CUE_HOT float surface_at(const CueWorld *w, float x, float z) {
    float ax = x < 0 ? -x : x, az = z < 0 ? -z : z;
    /* The bounding box first, because it rejects almost everything and costs
     * two compares; then the shape, which for a rectangular table is the same
     * question asked once more and for an L is the only one that is right. */
    if (ax > w->bound_x || az > w->bound_z) return CUE_NO_SURFACE;
    if (w->nbound && !cue_rects_contain(w->bound_r, w->nbound, x, z))
        return CUE_NO_SURFACE;
    int on_bed = w->nplay || w->nbedv ? cue_world_on_bed(w, x, z)
                          : (ax <= w->play_x && az <= w->play_z);

    for (int p = 0; p < w->npocket; p++) {
        /* THE HOLE IS THE CATCH. Same centre, same radius, deliberately —
         * these two are the only things that can disagree about whether a ball
         * is still on the table, and when they do it falls through the world.
         * They were a circle apart the moment the drop was allowed to sit
         * deeper than the pocket: the hole stayed on w->pocket while the catch
         * moved back to drop_c, leaving a crescent with no floor and nothing to
         * take the ball. Four per thousand on the 9 ft table went through it. */
        float dx = x - w->drop_c[p].x, dz = z - w->drop_c[p].z;
        if (dx * dx + dz * dz < w->pocket_r[p] * w->pocket_r[p]) return CUE_NO_SURFACE;

        /* AND THE BED STOPS WHERE THE CLOTH IS CUT, not at the cushion line.
         *
         * The playing area is a RECTANGLE and the cloth is not: each pocket
         * takes a scalloped bite out of it. This used to return "bed, height
         * zero" for the whole rectangle, so inside that bite a ball rolled on —
         * and came to rest on — felt the renderer had already drawn rolling
         * away. It also meant the hole below was unreachable from anywhere on
         * the table, because the rectangle answered first.
         *
         * Inside the rectangle the piece of the cut facing the table is always
         * the ARC, so this is one circle test rather than the whole boundary. A
         * ball that finds no bed here is over the mouth, and check_pockets pots
         * it on the same test — no cloth under it means potted. */
        if (on_bed) {
            float cx = x - w->cut_c[p].x, cz = z - w->cut_c[p].z;
            if (cx * cx + cz * cz < w->cut_r[p] * w->cut_r[p]) return CUE_NO_SURFACE;
        }
    }
    /* Outside the rectangle, cushion top and wood top are ONE FLAT LEVEL at
     * rail_top — that is how cue_render builds them (rail_h = flat_h), and a
     * ball running the frame on a jump shot lands on it. */
    return on_bed ? 0.0f : w->rail_top;
}

/* DID THE CUE BALL PASS OVER A BALL, and was it allowed to?
 *
 * Called every substep while the white is off the bed. "Passes over any part
 * of an object ball" is a footprint test: the two circles overlap in plan while
 * one of them is in the air. Whether that is a FOUL depends entirely on when it
 * happened relative to the first contact, which is why this has to be watched
 * as it happens rather than reconstructed at the settle.
 *
 *   before any contact  → provisionally a jump shot, unless exception (b)
 *                         rescues it at the landing
 *   after the contact,
 *     over another ball → exception (a), legal
 *     over that ball,
 *       after a cushion
 *       or another ball → exception (c), legal
 *       otherwise       → a jump shot
 */
static CUE_HOT void jump_watch(CueWorld *w, CueBall *balls, int n) {
    const CueBall *cue = &balls[0];
    const float cue_r = cue_ball_r(w, cue);
    for (int j = 1; j < n; j++) {
        if (!balls[j].on || balls[j].drop > 0.0f) continue;
        float reach = cue_r + cue_ball_r(w, &balls[j]);
        float dx = cue->pos.x - balls[j].pos.x;
        float dz = cue->pos.z - balls[j].pos.z;
        if (dx > reach || dx < -reach || dz > reach || dz < -reach) continue;
        if (dx * dx + dz * dz >= reach * reach) continue;   /* no overlap in plan */

        if (w->first_hit_idx >= 0) {
            if (j != w->first_hit_idx) continue;            /* (a) — over another ball */
            if (w->jmp_bounced) continue;                   /* (c) — off a cushion or ball */
            w->jump_over = 1; w->jump_over_id = balls[j].id;
            return;
        }
        /* Nothing struck yet. Hold it: (b) can still excuse this one, but only
         * the landing knows. */
        if (!w->jmp_pending) { w->jmp_pending = 1; w->jmp_idx = j; w->jmp_hit_it = 0; }
    }
}

/* The landing settles a pass-over that began before any contact.
 *
 * (b) excuses it when the cue ball jumped, hit that ball, and came down NOT on
 * the far side of it — you went into the ball, not past it. Anything else that
 * passed over a ball before touching one is a jump shot. */
static void jump_land(CueWorld *w, CueBall *balls, int n) {
    if (!w->jmp_pending) return;
    int j = w->jmp_idx;
    int excused = 0;
    if (w->jmp_hit_it && j >= 0 && j < n) {
        /* "not on the far side of the CURRENT position of that object ball",
         * measured along the way the cue ball was going. */
        float tx = balls[0].vel.x, tz = balls[0].vel.z;
        float l = sqrtf(tx * tx + tz * tz);
        if (l > 1e-5f) {
            tx /= l; tz /= l;
            float ox = balls[0].pos.x - balls[j].pos.x;
            float oz = balls[0].pos.z - balls[j].pos.z;
            if (ox * tx + oz * tz <= 0.0f) excused = 1;
        } else excused = 1;      /* stopped dead on it: certainly not past it */
    }
    if (!excused) {
        w->jump_over = 1;
        if (j >= 0 && j < n) w->jump_over_id = balls[j].id;
    }
    w->jmp_pending = 0; w->jmp_idx = -1; w->jmp_hit_it = 0;
}

static CUE_HOT void substep(CueWorld *w, CueBall *balls, int n, float h, uint32_t *ev) {
    /* 0. THE SKITTLES, WHICH ARE RIGID BODIES AND NOT AN ANIMATION.
     *
     * mote's solver does the work — gravity, the bed, the cushions, one pin
     * against another — and all this has to do is run it on the same clock as
     * the balls and then read what happened. A pin is DOWN when its own axis
     * has left upright far enough that it cannot come back, which is the same
     * test the engine's own domino example uses, and the rules care about the
     * ORDER they went in (Rule 112), so that is booked the moment each one
     * goes rather than counted up at the end. */
    if (w->sk_on && s_rigid && w->nskittle > 0) {
        s_rigid(&w->sk_world, w->sk, w->sk_n, h);
        for (int k = 0; k < w->nskittle; k++) {
            /* the pin's axis against the vertical: 0.7 is about 45 degrees */
            if (!w->skittle_down[k] && w->sk[k].orient.r[1].y < 0.70f) {
                w->skittle_down[k] = 1;
                w->skittle_order[k] = (uint8_t)(++w->skittle_fell);
            }
            /* RULE 103 IS DECIDED BY WHERE IT ENDS UP, not by how hard it was
             * hit. A pin that has been moved off its spot and is still on its
             * feet is a nudge: the score counts and it is replaced before the
             * next stroke. That used to be a speed threshold at the moment of
             * contact, which was a guess standing in for an outcome; now the
             * body has an outcome and it can simply be read. */
            if (!w->skittle_down[k]) {
                const float dx = w->sk[k].pos.x - w->skittle_spot[k].x;
                const float dz = w->sk[k].pos.z - w->skittle_spot[k].z;
                if (dx*dx + dz*dz > 0.0015f * 0.0015f) w->skittle_nudged[k] = 1;
            }
            /* ...and where it is now, for the drawing and for the respot. */
            w->skittle[k].x = w->sk[k].pos.x;
            w->skittle[k].z = w->sk[k].pos.z;
        }
    }

    /* 1. cloth friction + integrate. Balls mid-drop instead fall into the
     * pocket (pulled to the centre + accelerating downward) and are removed
     * when they sink below the recess. */
    for (int i = 0; i < n; i++) {
        CueBall *b = &balls[i];
        if (!b->on) continue;
        if (b->drop > 0.0f) {
            /* IN THE POCKET, FALLING — AND THE LIP STEERS IT IN.
             *
             * The cloth rolls over the edge of the cut, and by the time a ball
             * is above that roll it is already falling: its centre is past the
             * slate and nothing is holding it up. So the lip is not something
             * to stand on. What it does is DIRECT — it gathers the ball toward
             * the middle of the pocket on the way down, which is why a ball
             * that catches the near edge still drops instead of hanging.
             *
             * That steer was missing. The sideways motion was multiplied by
             * 0.94 EVERY SUBSTEP, which at two thousand a second is gone
             * inside a millisecond, so the ball stopped dead and sank
             * vertically from wherever it crossed — straight down through the
             * cloth that is drawn rolling underneath it. On a firm pot nobody
             * sees it; on a ball dying into the pocket it is the whole
             * picture.
             *
             * Now it keeps its pace and is pulled to the pocket's axis as it
             * falls, so it goes in the way it goes in on a table: leaning into
             * the middle, not dropping through the felt. */
            int   pk = b->pocket;
            float ld = w->lip_d[pk], rr = ld + cue_ball_r(w, b);
            float o0 = cue_phys_cut_out(w, pk, b->pos.x, b->pos.z);

            Vec3 pc2 = w->drop_c[pk];
            /* GATHERED, NOT SPRUNG.
             *
             * The pull toward the pocket's axis is a spring, and a spring with
             * no damping in it does what springs do: the ball was thrown across
             * the throat, came back, and crossed again all the way down. At
             * CUE_LIP_GATHER = 90 the natural frequency is sqrt(90) = 9.5 rad/s
             * and the drag here was 3/s, where not oscillating at all needs
             * 2*sqrt(90) = 19/s — so it was a long way underdamped, and it
             * showed: measured over 180 pots per table, a ball reversed
             * direction inside a snooker middle 107 TIMES and took 582 ms to
             * sink. Dropping the back wall's restitution helped that barely at
             * all, because the wall was never what was throwing it about.
             *
             * A pocket does not play the ball back at you. It takes what the
             * ball has and lets it fall, so the gather is damped just past
             * critical: it steers to the middle and stays there. */
            {   const float kk = CUE_LIP_GATHER;
                const float cd = 2.2f * sqrtf(kk);       /* 1.1x critical */
                b->vel.x += ((pc2.x - b->pos.x) * kk - b->vel.x * cd) * h;
                b->vel.z += ((pc2.z - b->pos.z) * kk - b->vel.z * cd) * h; }
            b->vel.y -= w->g * h;
            b->pos.x += b->vel.x * h;
            b->pos.y += b->vel.y * h;
            b->pos.z += b->vel.z * h;

            /* AND THE LIP IS STILL UNDER IT. The cloth rolls over the edge in a
             * quarter circle of radius `ld`, so a ball on that roll has its
             * centre on an arc of radius ld+R about the same point: it keeps
             * its pace across, and it goes down at the rate the cloth falls
             * away from it, not at the rate gravity would take it. Only once
             * its centre is past the roll — a distance ld+R out — is there
             * nothing left underneath and it drops properly.
             *
             * Which is also why a firm pot looks different from a dying one.
             * The roll only holds a ball that is slower than sqrt(g(ld+R)),
             * about 0.7 m/s; anything above that leaves the edge and flies, and
             * that falls out of the same two lines rather than being asked for. */
            {
                float o1 = cue_phys_cut_out(w, pk, b->pos.x, b->pos.z);
                /* ...WHERE THERE IS A RIM UNDER IT TO BE HELD BY.
                 *
                 * The arc above describes cloth turning over the EDGE of the
                 * cut. Once the whole ball is inside the throat wall there is
                 * no edge below it — it is over open pocket, and the only
                 * honest thing beneath it is air. Saying so costs one compare
                 * and makes "a ball parked on top of a pocket" a shape the
                 * physics cannot express, rather than one it merely happens
                 * not to reach. cue_table_derive_cut is the actual fix (it
                 * caps the roll against the pocket's own size); this is why
                 * that fix cannot fail quietly. */
                float gx = b->pos.x - pc2.x, gz = b->pos.z - pc2.z;
                float clear = w->pocket_r[pk] - cue_ball_r(w, b) * 1.35f;
                int over_open = clear > 0.0f && (gx*gx + gz*gz) < clear * clear;
                if (!over_open && o1 > 0.0f && o1 < rr * 0.999f) {
                    float lip_y = sqrtf(rr*rr - o1*o1) - ld;
                    if (b->pos.y < lip_y) {
                        b->pos.y = lip_y;
                        const float slope = -o1 / sqrtf(rr*rr - o1*o1);  /* dy/do <= 0 */
                        /* GRAVITY GETS TO WORK ON IT, which it did not.
                         *
                         * The vertical speed used to be set outright from how
                         * fast the ball was crossing sideways —
                         * slope * (o1-o0)/h — so a ball creeping over the edge
                         * descended at a creep however steep the roll had got
                         * under it, and vel.y was overwritten every substep so
                         * gravity never accumulated. Reported as pots hanging
                         * over the pocket as if suspended, and measured: a ball
                         * arriving at 80 mm/s took 442 ms to disappear into a
                         * 7 ft corner against 154 ms of free fall — nearly
                         * three tenths of a second of hanging.
                         *
                         * A ball on a curved surface is not driven by its own
                         * sideways speed; it is ACCELERATED down the slope by
                         * gravity, and it leaves when it outruns the curve. So
                         * take gravity's component along the surface — and the
                         * rolling 5/7 of it, because it is still on cloth — and
                         * push the ball outward with it. The vertical speed then
                         * follows from the constraint rather than replacing it,
                         * and a ball parked on the lip accelerates away instead
                         * of sitting there.
                         *
                         * WHICH WAY IS "OUTWARD" is asked of the cut itself, not
                         * assumed to be radial: the cloth edge is an arc with two
                         * straight ends (see cue_phys_cut_out) and its gradient
                         * is the only honest answer near the joins. */
                        {   const float eps = 1e-4f;
                            const float gx =
                                cue_phys_cut_out(w, pk, b->pos.x + eps, b->pos.z) -
                                cue_phys_cut_out(w, pk, b->pos.x - eps, b->pos.z);
                            const float gz =
                                cue_phys_cut_out(w, pk, b->pos.x, b->pos.z + eps) -
                                cue_phys_cut_out(w, pk, b->pos.x, b->pos.z - eps);
                            const float gl = sqrtf(gx*gx + gz*gz);
                            if (gl > 1e-9f) {
                                /* tangential gravity, resolved back onto the
                                 * horizontal: -g*y'/(1+y'^2), rolling */
                                const float at = (5.0f/7.0f) * w->g * (-slope)
                                               / (1.0f + slope*slope);
                                b->vel.x += (gx / gl) * at * h;
                                b->vel.z += (gz / gl) * at * h;
                            } }
                        /* AND THE CONSTRAINT ON THE WAY DOWN, which is all the
                         * old line was entitled to do: the surface may stop the
                         * ball falling THROUGH it, and it may not hold it up.
                         * The rate the cloth falls away is the floor on the
                         * descent, not the value of it — a ball already dropping
                         * faster than the roll keeps its own speed and leaves. */
                        const float rate = slope * (o1 - o0) / h;
                        if (b->vel.y > rate) b->vel.y = (rate < 0.0f) ? rate : 0.0f;
                    }
                }
            }
            /* AND THE THROAT HAS A BACK.
             *
             * A ball arriving at pace is still travelling when it crosses the
             * drop, and until now nothing stopped it: it kept its speed, fell
             * as it went, and left the pocket out the far side — counted as
             * potted and then visibly rolling away down the outside of the
             * table. Measured before this: 666 of 702 hard pots strayed past
             * the rim, the worst by 450 mm at 9 m/s.
             *
             * A real pocket is a shaped casting with leather behind it. It is
             * what takes the pace off a firm pot and drops it in rather than
             * letting it through, and it is the same surface the ball rattles
             * off when it does not go. So: a wall at the throat radius, which
             * the ball cannot pass and gives up most of its pace against. */
            {
                float bx = b->pos.x - pc2.x, bz = b->pos.z - pc2.z;
                float rmax = w->pocket_r[pk] - cue_ball_r(w, b) * 0.35f;
                if (rmax < cue_ball_r(w, b) * 0.25f) rmax = cue_ball_r(w, b) * 0.25f;
                float q2 = bx*bx + bz*bz;
                if (q2 > rmax * rmax) {
                    float q = sqrtf(q2);
                    float nx = bx / q, nz = bz / q;
                    b->pos.x = pc2.x + nx * rmax;
                    b->pos.z = pc2.z + nz * rmax;
                    float vn = b->vel.x * nx + b->vel.z * nz;
                    if (vn > 0.0f) {              /* only what is heading out */
                        /* A pocket back is leather over a shaped casting and
                         * gives back almost nothing — which is the point of it.
                         * At 0.25 a ball caught off-centre was thrown across the
                         * throat by the gather, bounced, and rattled visibly on
                         * the way down: two contacts and 445 ms at a middle,
                         * where the throat is narrower and the catch sits deeper.
                         * A real one deadens the ball, it does not play it. */
                        const float e = 0.06f;
                        b->vel.x -= (1.0f + e) * vn * nx;
                        b->vel.z -= (1.0f + e) * vn * nz;
                    }
                }
            }
            ball_spin_orient(b, h);
            if (b->pos.y < CUE_POCKET_FLOOR) { b->on = 0; b->drop = 0.0f; }
            continue;
        }
        if (0) {
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
        /* IN THE AIR: gravity, and NOTHING ELSE. No cloth contact, because
         * there is no cloth contact — which is the whole character of a jumped
         * ball. Its spin is preserved through the flight instead of being
         * turned into roll, so it lands still sliding and skids on, and that is
         * why a jump shot behaves nothing like the same stroke played flat.
         *
         * It is also why this must not happen by accident: a ball that hops
         * unintentionally stops developing roll, and every draw and follow shot
         * quietly stops meaning what it meant. The deadband that prevents that
         * lives at the strike, not here. */
        /* OFF THE BED: in the air, or running along the rail.
         *
         * The surface under it decides which. A ball over the rail falls to the
         * rail and rolls on it; roll back inside the cushion line and the
         * surface drops away and it falls to the cloth; roll off the outside,
         * or over a pocket mouth, and there is nothing under it at all. */
        float surf = surface_at(w, b->pos.x, b->pos.z);
        /* THE RAIL ONLY HOLDS A BALL THAT IS ALREADY ON TOP OF IT.
         *
         * A ball at cloth height whose centre has crossed the cushion line is
         * not on the rail — it is IN the cushion, mid-bounce, and the cushion
         * push-out is about to put it back. Every hard shot does this: the ball
         * penetrates the rail by a millimetre or two before the collision
         * resolves. Treating that as "over the rail region, so it must be on
         * the rail" teleported it up onto the frame, from where it rolled
         * gently off the table and was deleted — a ball lost out of the jaws
         * on hard shots, which is exactly what was reported.
         *
         * Below the cap, the answer is the bed, and the cushion does the rest. */
        if (surf > 0.0f && b->pos.y < surf + cue_ball_r(w, b) - 1.0e-4f) surf = 0.0f;
        float rest_y = (surf > CUE_NO_SURFACE * 0.5f) ? surf + cue_ball_r(w, b) : -1.0e9f;
        if (b->pos.y > rest_y + 1.0e-5f || b->vel.y > 0.0f) {
            b->vel.y -= w->g * h;
            b->pos = v3_add(b->pos, v3_scale(b->vel, h));
            /* AIR RESISTANCE IS NOT THE POINT — a ball skidding along a rail is.
             * While it is off the cloth it still meets whatever it is over, so
             * the horizontal motion is left to the surface below. */
            if (b->pos.y <= rest_y) {
                b->pos.y = rest_y;
                if (b->vel.y < -w->v_land) b->vel.y = -b->vel.y * w->e_bed;
                else                       b->vel.y = 0.0f;
                s_bed_land = 1;
                if (i == 0 && surf == 0.0f) jump_land(w, balls, n);
            }
            /* GONE means past the frame, or below the floor of the world after
             * falling through a pocket mouth from the rail — not merely past
             * the cushion. Deleting it at the cushion line, which is what this
             * did, removes a shot that is still happening: a ball is allowed to
             * land on the rail, run along it, and come back down. */
            if (b->pos.x >  w->bound_x || b->pos.x < -w->bound_x ||
                b->pos.z >  w->bound_z || b->pos.z < -w->bound_z ||
                (w->nbound && !cue_rects_contain(w->bound_r, w->nbound,
                                                 b->pos.x, b->pos.z)) ||
                b->pos.y < -0.12f) {
                b->on = 0; b->drop = 0.0f;
                b->pocket = CUE_OFF_TABLE;
                b->vel = v3(0, 0, 0); b->w = v3(0, 0, 0);
                continue;
            }
            ball_spin_orient(b, h);
            continue;
        }
        if (surf > 0.0f) {
            /* RESTING ON THE RAIL. It rolls up here exactly as it would on the
             * cloth — same friction, same spin — and the height is held so it
             * does not sink through. A ball that has genuinely stopped up here
             * is not coming back, and a frame cannot wait on it: ten seconds
             * and it has left the table, which is what a referee would say. */
            b->pos.y = rest_y;
            b->astray += h;
            if (b->astray > 10.0f) {
                b->on = 0; b->drop = 0.0f;
                b->pocket = CUE_OFF_TABLE;
                b->vel = v3(0, 0, 0); b->w = v3(0, 0, 0);
                continue;
            }
            ball_cloth(w, b, h);
            b->pos = v3_add(b->pos, v3_scale(b->vel, h));
            b->pos.y = rest_y;
            ball_spin_orient(b, h);
            continue;
        }
        /* ON THE CLOTH clears the stuck timer; anywhere else runs it, including
         * the millisecond a ball spends inside a cushion mid-bounce — which is
         * nowhere near ten seconds, so a bounce costs nothing and a ball wedged
         * where it cannot be played still leaves. */
        if (w->nplay || w->nbedv ? cue_world_on_bed(w, b->pos.x, b->pos.z)
                     : (b->pos.x <= w->play_x && b->pos.x >= -w->play_x &&
                        b->pos.z <= w->play_z && b->pos.z >= -w->play_z))
            b->astray = 0.0f;
        else {
            b->astray += h;
            if (b->astray > 10.0f) {
                b->on = 0; b->drop = 0.0f;
                b->pocket = CUE_OFF_TABLE;
                b->vel = v3(0, 0, 0); b->w = v3(0, 0, 0);
                continue;
            }
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
        b->pos.y = cue_ball_r(w, b);
        ball_spin_orient(b, h);
    }
    /* 1b. the jump-shot watch, while the white is off the bed. Before the
     * collisions, so a pass-over is seen at the position it happened at rather
     * than after the contact has moved everything. */
    if (n > 0 && balls[0].on && cue_phys_airborne(w, &balls[0]))
        jump_watch(w, balls, n);

    /* 2. ball–ball (skip droppers). Record the CUE ball's (index 0) first
     * object-ball contact for the rules. A cheap per-axis broad-phase reject
     * (exactly equivalent to the dist>=2R early-out, but no sqrt) skips the far
     * pairs — a big win on snooker's 22 balls (O(n^2) pairs per substep). */
    /* The reject has to be at least the biggest pair on the table, or it would
     * skip a real contact. With a smaller cue ball every pair is 2R or less, so
     * the set's own diameter is still the safe bound. */
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
                if (i == 0) touch_add(w, CUE_TOUCH_BALL, balls[j].id, (uint8_t)j);
                if (w->first_hit < 0 && i == 0) { w->first_hit = balls[j].id; w->first_hit_idx = j; }
                else if (w->first_hit >= 0 && i == 0) w->jmp_bounced = 1;  /* (c) */
                /* Did it hit the ball it is in the act of passing over? That is
                 * the whole of exception (b), decided at the landing. */
                if (i == 0 && w->jmp_pending && j == w->jmp_idx) w->jmp_hit_it = 1;
                if (i == 0 && w->jmp_pending && j != w->jmp_idx) w->jmp_hit_it = 0;
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
        uint32_t cev = 0;
        if (collide_cushions(w, b, ev ? ev : &cev)) {
            /* The cue ball's own account. Recorded whether or not it has hit a
             * ball yet: a carom counts every cushion from the start of the
             * shot, not only the ones after first contact. */
            if (i == 0) touch_add(w, CUE_TOUCH_CUSHION, 0, 0);
            if (i == 0 && w->first_hit >= 0) w->jmp_bounced = 1;      /* (c) */
            /* Book the side-cushion fact on the shot (Rule 108's witness):
             * the collision path is const, so it arrives as an event bit. */
            if ((ev ? *ev : cev) & CUE_EV_SIDE_CUSH) w->side_cushion = 1;
        }
        if (check_pockets(w, b) && ev) *ev |= CUE_EV_POCKET;
        if (check_skittles(w, b) && ev) *ev |= CUE_EV_SKITTLE;
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

CUE_HOT int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events) {
    if (events) *events = 0;
    s_cush_vn = 0.0f;                  /* reset the cushion-impact meter for this step */
    s_bed_land = 0;
    float h = g_sub_h;
    w->_acc += dt;
    int iters = 0;
    while (w->_acc >= h && iters < CUE_MAX_SUB) {
        substep(w, balls, n, h, events);
        w->_acc -= h;
        iters++;
    }
    if (iters >= CUE_MAX_SUB) w->_acc = 0.0f;   /* shed backlog */
    if (s_bed_land && events) *events |= CUE_EV_BED;

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

