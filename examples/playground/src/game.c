/*
 * playground — a physics PLAYGROUND showcasing STATIC colliders + per-body
 * materials. A ground plane, two tilted brown ramps and low grey perimeter
 * walls are all immovable (inv_mass = 0). A pool of dynamic balls drops onto
 * the ramps; the balls come in three MATERIALS whose behaviour you can see:
 *
 *   - BOUNCY  (red)   restitution 0.9   -> springs back up off the ramp/ground
 *   - ICE     (cyan)  friction 0.02     -> barely grips, slides far down-slope
 *   - DEFAULT (gold)  world material     -> rolls/settles normally
 *
 * Controls: A spawn/respawn a ball at the top of a ramp (small push);
 *           B reset all; MENU exit.
 */
#include "mote_api.h"
#include "pg_meshes.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* Body layout in the array: statics first (fixed indices), then the dynamic
 * ball pool. The solver treats inv_mass==0 bodies as immovable colliders. */
#define MAT_BOUNCY  0
#define MAT_ICE     1
#define MAT_DEFAULT 2

#define NBALL      8                 /* dynamic ball pool */
#define I_GROUND   0
#define I_RAMP_L   1
#define I_RAMP_R   2
#define I_WALL0    3                 /* 4 perimeter walls */
#define NSTATIC    7                 /* ground + 2 ramps + 4 walls */
#define NTOTAL     (NSTATIC + NBALL)

static MoteWorld world;
static MoteBody  body[NTOTAL];
static int       s_mat[NBALL];       /* material of each pool ball */
static int       s_live = 0;         /* how many balls have been spawned */
static int       s_next = 0;         /* round-robin index into the pool */
static uint32_t  rng = 1u;
static Vec3      cam_pos;
static Mat3      cam_basis;

/* Ramp geometry (must match k_slab_mesh proportions: half 2.0 x 0.205 x 1.40). */
static const Vec3 RAMP_HALF = { 2.0f, 0.205f, 1.40f };
static const float RAMP_TILT = 0.44f;   /* ~25 degrees */

static char *ap_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_i(char *p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
static float frand(void) {       /* xorshift -> [-1,1) */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
}

/* The two ramps form a shallow VALLEY: each tilts so its OUTER edge is high
 * and its inner edge (near the centre) is low. A ball dropped on the outer
 * high edge rolls DOWN toward the centre, where the two ramps meet, and the
 * balls collect there instead of rolling off. Rotation is about world Z. */
static Mat3 ramp_orient(int left) {
    Mat3 m = m3_identity();
    /* left ramp: high edge at -X -> tilt so +X goes down (negative about Z);
     * right ramp mirrors. */
    m3_rotate_local(&m, 2, left ? -RAMP_TILT : RAMP_TILT);
    return m;
}

static void make_static(void) {
    /* Ground: infinite plane half-space, normal = orient.r[1] = +Y. */
    MoteBody *g = &body[I_GROUND];
    *g = (MoteBody){0};
    g->shape   = MOTE_SHAPE_PLANE;
    g->pos     = v3(0, 0, 0);
    g->orient  = m3_identity();
    g->inv_mass = 0.0f;
    g->radius  = 0.0f;

    /* Two tilted ramp slabs. Centre lowered so the slab sits just above the
     * ground, high edge raised by the tilt. Left ramp left of centre. */
    for (int s = 0; s < 2; s++) {
        MoteBody *r = &body[I_RAMP_L + s];
        int left = (s == 0);
        *r = (MoteBody){0};
        r->shape    = MOTE_SHAPE_BOX;
        r->half     = RAMP_HALF;
        r->radius   = v3_len(RAMP_HALF);
        r->orient   = ramp_orient(left);
        r->inv_mass = 0.0f;
        /* Place each ramp's centre off to its side; raise centre so the low
         * edge clears the ground (slab tilts ~25 deg over a 2 m half-width). */
        float cx = left ? -1.9f : 1.9f;
        r->pos = v3(cx, 1.05f, 0.0f);
    }

    /* Four low perimeter walls boxing in the play area (~+-4 m). */
    struct { Vec3 pos; int along_x; } w[4] = {
        { v3(0.0f,  0.4f, -4.0f), 1 },   /* far  (runs along X) */
        { v3(0.0f,  0.4f,  4.0f), 1 },   /* near (along X) */
        { v3(-4.0f, 0.4f,  0.0f), 0 },   /* left (along Z) */
        { v3( 4.0f, 0.4f,  0.0f), 0 },   /* right(along Z) */
    };
    for (int i = 0; i < 4; i++) {
        MoteBody *b = &body[I_WALL0 + i];
        *b = (MoteBody){0};
        b->shape    = MOTE_SHAPE_BOX;
        /* k_wall_mesh half-extents: 2.4 x 0.4 x 0.18 (scale 2.4). */
        Vec3 half = w[i].along_x ? v3(2.4f, 0.4f, 0.18f) : v3(0.18f, 0.4f, 2.4f);
        b->half     = half;
        b->radius   = v3_len(half);
        if (w[i].along_x) b->orient = m3_identity();
        else { Mat3 m = m3_identity(); m3_rotate_local(&m, 1, 1.5707963f); b->orient = m; }
        b->inv_mass = 0.0f;
        b->pos = w[i].pos;
    }
}

/* Spawn one ball into pool slot `slot` at the top (high edge) of a ramp,
 * with a small push down-slope. Cycles materials so the mix is visible. */
static void spawn_ball(int slot) {
    MoteBody *b = &body[NSTATIC + slot];
    int mat = slot % 3;
    s_mat[slot] = mat;

    *b = (MoteBody){0};
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = 0.26f;
    b->inv_mass = 1.0f / 0.3f;
    b->orient   = m3_identity();

    switch (mat) {
        case MAT_BOUNCY:  b->restitution = 0.9f;  b->friction = 0.0f;  break;
        case MAT_ICE:     b->restitution = 0.0f;  b->friction = 0.02f; break;
        default:          b->restitution = 0.0f;  b->friction = 0.0f;  break; /* world */
    }

    /* Alternate which ramp's high edge we drop from. Left ramp high edge is
     * at -X; right ramp high edge at +X. Drop a little above the surface. */
    int left = (slot & 1) == 0;
    /* Drop on the OUTER (high) edge of a ramp: left ramp high edge at -X,
     * right ramp at +X. The ball then rolls down toward the centre. */
    float hi_x = left ? -3.4f : 3.4f;
    b->pos = v3(hi_x + frand() * 0.2f,
                3.2f,
                frand() * 0.7f);
    /* Small push down-slope (toward centre). */
    b->vel = v3(left ? 0.7f : -0.7f, 0.0f, 0.0f);
    b->_reserved[0] = 0;                          /* wake */

    if (slot >= s_live) s_live = slot + 1;
}

static void reset_all(void) {
    /* Park every pool ball far below/out of sight (inv_mass 0 so the solver
     * ignores them) and clear the live count. */
    for (int i = 0; i < NBALL; i++) {
        MoteBody *b = &body[NSTATIC + i];
        *b = (MoteBody){0};
        b->shape   = MOTE_SHAPE_SPHERE;
        b->radius  = 0.26f;
        b->inv_mass = 0.0f;
        b->orient  = m3_identity();
        b->pos     = v3(0.0f, -50.0f, 0.0f);
    }
    s_live = 0;
    s_next = 0;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120, 165, 210));   /* sky */
    mote->scene_set_sun(v3(0.45f, 0.85f, -0.3f));

    mote->phys_world_defaults(&world);
    world.walls   = 0;                       /* NO auto box; we build statics */
    world.gravity = v3(0.0f, -9.8f, 0.0f);
    world.substep = 1.0f / 240.0f;
    world.max_substeps = 8;
    /* World defaults for the "default" material balls. */
    world.restitution = 0.25f;
    world.friction    = 0.5f;

    rng = (uint32_t)mote->micros() | 1u;
    make_static();
    reset_all();

    /* Seed a few balls so the scene shows material variety immediately. */
    for (int i = 0; i < 6; i++) spawn_ball(i);
    s_next = 6 % NBALL;

    /* Camera: off to the side and above, tilted down to view the ramps. */
    cam_pos = v3(0.0f, 3.4f, -7.6f);
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.34f);   /* pitch down */
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_B))    reset_all();
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        spawn_ball(s_next);
        s_next = (s_next + 1) % NBALL;
    }

    mote->phys_step(&world, body, NTOTAL, dt);

    mote->scene_begin(&cam_basis, 58.0f);

    /* Ground (the PLANE has no geometry — draw a big quad at y=0). */
    MoteObject ground = { .pos = v3_sub(v3(0, 0, 0), cam_pos),
                          .basis = m3_identity(), .mesh = &k_ground_mesh };
    mote->scene_add_object(&ground);

    /* Ramps. */
    for (int s = 0; s < 2; s++) {
        MoteBody *r = &body[I_RAMP_L + s];
        MoteObject o = { .pos = v3_sub(r->pos, cam_pos),
                         .basis = r->orient, .mesh = &k_slab_mesh };
        mote->scene_add_object(&o);
    }
    /* Walls. */
    for (int i = 0; i < 4; i++) {
        MoteBody *b = &body[I_WALL0 + i];
        MoteObject o = { .pos = v3_sub(b->pos, cam_pos),
                         .basis = b->orient, .mesh = &k_wall_mesh };
        mote->scene_add_object(&o);
    }

    /* Balls: colour by material so the difference is visible. */
    static const uint16_t mat_col[3] = {
        MOTE_RGB565(235, 70, 70),    /* BOUNCY  red   */
        MOTE_RGB565(90, 220, 235),   /* ICE     cyan  */
        MOTE_RGB565(240, 200, 70),   /* DEFAULT gold  */
    };
    for (int i = 0; i < NBALL; i++) {
        MoteBody *b = &body[NSTATIC + i];
        if (b->inv_mass == 0.0f) continue;       /* parked / not spawned */
        Vec3 p = v3_sub(b->pos, cam_pos);
        mote->scene_add_sphere(p, b->radius, mat_col[s_mat[i]]);
    }
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "PLAYGROUND", 3, 3, MOTE_RGB565(255, 255, 255));

    char line[24], *p = line;
    p = ap_s(p, "BALLS ");
    p = ap_i(p, s_live);
    *p = 0;
    mote->text(fb, line, 3, 12, MOTE_RGB565(120, 220, 255));

    /* Tiny material legend. */
    mote->text(fb, "RED bouncy",  3, 108, MOTE_RGB565(235, 70, 70));
    mote->text(fb, "CYAN ice",    3, 116, MOTE_RGB565(90, 220, 235));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
