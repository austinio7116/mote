/*
 * dominoes — a knock-down chain showcasing Mote's box dynamics and the
 * stacking solver: a row of thin upright domino boxes the solver holds
 * standing until a heavy ball, launched along the row, topples the chain.
 *
 * Controls: A launches the ball, B resets (stands the dominoes back up and
 * parks the ball), MENU exits.
 */
#include "mote_api.h"
#include "meshes.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NDOM   12                 /* dominoes in the row */
#define IBALL  NDOM               /* ball is the last body */
#define NBODY  (NDOM + 1)

/* Domino half-extents: thin in X (topple axis), tall in Y, broad in Z. */
#define DOM_HX  0.05f
#define DOM_HY  0.30f
#define DOM_HZ  0.18f
#define DOM_SP  0.28f             /* spacing along X (centre to centre) */
#define BALL_R  0.22f             /* heavy launch ball radius */

static MoteWorld world;
static MoteBody  body[NBODY];

static float row_x0(void) { return -(NDOM - 1) * 0.5f * DOM_SP; }

/* Stand a single domino bolt-upright on the floor at its row slot. */
static void stand_domino(int i) {
    MoteBody *b = &body[i];
    b->shape    = MOTE_SHAPE_BOX;
    b->half     = v3(DOM_HX, DOM_HY, DOM_HZ);
    b->radius   = 0.36f;                  /* bounding radius for broad phase */
    b->inv_mass = 1.0f / 0.10f;           /* light: a heavy ball flattens it */
    b->pos      = v3(row_x0() + i * DOM_SP, DOM_HY, 0.0f);  /* base on floor */
    b->vel      = v3(0, 0, 0);
    b->w        = v3(0, 0, 0);
    b->orient   = m3_identity();          /* exactly upright */
    b->friction = 0.6f;
    b->restitution = 0.0f;
    b->_reserved[0] = 0;                  /* wake */
}

/* Park the ball behind the first domino, motionless. */
static void park_ball(void) {
    MoteBody *b = &body[IBALL];
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = BALL_R;
    b->half     = v3(0, 0, 0);
    b->inv_mass = 1.0f / 1.2f;            /* heavy relative to the dominoes */
    b->pos      = v3(row_x0() - 0.6f, BALL_R, 0.0f);
    b->vel      = v3(0, 0, 0);
    b->w        = v3(0, 0, 0);
    b->orient   = m3_identity();
    b->friction = 0.4f;
    b->restitution = 0.1f;
    b->_reserved[0] = 0;
}

static void reset_scene(void) {
    for (int i = 0; i < NDOM; i++) stand_domino(i);
    park_ball();
}

static void launch_ball(void) {
    body[IBALL].vel = v3(3.0f, 0.0f, 0.0f);   /* roll along +X into the row */
    body[IBALL]._reserved[0] = 0;             /* wake */
}

static Vec3 cam_pos;
static Mat3 cam_basis;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 16, 26));
    mote->scene_set_sun(v3(0.5f, 0.85f, -0.3f));

    mote->phys_world_defaults(&world);
    world.walls = 1;                          /* flat box arena, floor at y=0 */
    world.bmin  = v3(-3.0f, 0.0f, -1.2f);
    world.bmax  = v3( 3.0f, 4.0f,  1.2f);
    world.gravity     = v3(0.0f, -9.8f, 0.0f);
    world.restitution = 0.1f;                 /* clean topple, little bounce */
    world.substep     = 1.0f / 240.0f;        /* fine: keeps thin boxes upright */
    world.max_substeps = 6;

    reset_scene();

    /* 3/4 view down the row: off to one side, raised, looking at the row's
     * midpoint so both the standing row and the floor are visible. */
    cam_pos = v3(-2.6f, 1.4f, -2.4f);
    Vec3 target = v3(0.3f, 0.25f, 0.0f);      /* a touch past the row centre */
    Vec3 fwd = v3_norm(v3_sub(target, cam_pos));
    Vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
    Vec3 up = v3_cross(fwd, right);
    cam_basis.r[0] = right;
    cam_basis.r[1] = up;
    cam_basis.r[2] = fwd;
}

static int standing_count(void) {
    int n = 0;
    for (int i = 0; i < NDOM; i++)
        if (body[i].orient.r[1].y >= 0.7f) n++;   /* up-axis still near vertical */
    return n;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    launch_ball();
    if (mote_just_pressed(in, MOTE_BTN_B))    reset_scene();

    mote->phys_step(&world, body, NBODY, dt);

    mote->scene_begin(&cam_basis, 55.0f);

    /* Floor quad, scaled to cover the arena. */
    MoteObject floor = { .pos = v3_sub(v3(0, 0, 0), cam_pos),
                         .basis = m3_identity(), .mesh = &k_floor_mesh };
    mote->scene_add_object_scaled(&floor, 3.0f);

    /* Dominoes: each rendered with its physics orientation as the basis. */
    for (int i = 0; i < NDOM; i++) {
        MoteObject o = { .pos = v3_sub(body[i].pos, cam_pos),
                         .basis = body[i].orient, .mesh = &k_domino_mesh };
        mote->scene_add_object(&o);
    }

    /* The launch ball as a shaded impostor. */
    Vec3 bp = v3_sub(body[IBALL].pos, cam_pos);
    mote->scene_add_sphere(bp, BALL_R, MOTE_RGB565(235, 90, 80));
}

static char *ap_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_i(char *p, int v) {
    char t[8]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "DOMINOES - A:launch B:reset", 3, 3, MOTE_RGB565(255, 255, 0));

    char line[24], *p = line;
    p = ap_s(p, "STANDING ");
    p = ap_i(p, standing_count());
    p = ap_s(p, "/");
    p = ap_i(p, NDOM);
    *p = 0;
    mote->text(fb, line, 3, 118, MOTE_RGB565(120, 220, 255));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
