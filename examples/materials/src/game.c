/*
 * materials — an A/B showcase of PER-BODY restitution.
 *
 * Five identical balls are dropped from the SAME height onto a static floor,
 * each with a different bounce coefficient (0.0, 0.3, 0.6, 0.85, 0.97). They
 * settle to visibly different bounce heights — the dead one stays glued to the
 * floor, the lively one rockets back up. Colour runs dull -> bright with
 * restitution so the difference reads at a glance. The whole row re-drops every
 * few seconds so the bounce is always on screen.
 *
 * Controls: A re-drops now; MENU exits.
 */
#include "mote_api.h"
#include "mat_meshes.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NBALL  5

static MoteWorld world;
static MoteBody  body[NBALL];
static Vec3      cam_pos;
static Mat3      cam_basis;
static float     s_redrop_t;       /* seconds since last drop */

/* Per-ball restitution and a dull->bright colour gradient that tracks it. */
static const float    k_rest[NBALL] = { 0.00f, 0.30f, 0.60f, 0.85f, 0.97f };
static const uint16_t k_col[NBALL]  = {
    MOTE_RGB565( 90,  80,  80),    /* 0.00 — dead grey */
    MOTE_RGB565(150, 110,  70),    /* 0.30 — dull amber */
    MOTE_RGB565(210, 150,  60),    /* 0.60 — orange */
    MOTE_RGB565(245, 210,  70),    /* 0.85 — gold */
    MOTE_RGB565(120, 255, 160),    /* 0.97 — bright green */
};

#define BALL_R   0.30f             /* ball radius (m) */
#define DROP_Y   3.4f              /* common release height (m) */
#define SPACING  0.95f             /* X spacing between balls (m) */
#define FLOOR_Y  0.0f

/* String helpers (no libc in a game module). */
static char *ap_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static void drop(void) {
    for (int i = 0; i < NBALL; i++) {
        MoteBody *b = &body[i];
        b->shape       = MOTE_SHAPE_SPHERE;
        b->radius      = BALL_R;
        b->inv_mass    = 1.0f / 0.3f;
        b->restitution = k_rest[i];        /* PER-BODY override of world default */
        b->friction    = 0.4f;
        b->pos = v3((i - (NBALL - 1) * 0.5f) * SPACING, DROP_Y, 0.0f);
        b->vel = v3(0.0f, 0.0f, 0.0f);
        b->w   = v3(0.0f, 0.0f, 0.0f);
        b->orient = m3_identity();
        b->_reserved[0] = 0;               /* wake (clear sleep counter) */
    }
    s_redrop_t = 0.0f;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 14, 26));
    mote->scene_set_sun(v3(0.3f, 0.85f, -0.45f));

    mote->phys_world_defaults(&world);
    world.walls       = 0;                 /* no auto box; floor is a static PLANE */
    world.gravity     = v3(0.0f, -9.8f, 0.0f);
    world.restitution = 0.5f;              /* default; each ball overrides it */
    world.friction    = 0.4f;
    /* High substep RATE + headroom so the fast bounces resolve accurately and
     * don't tunnel through the floor. */
    world.substep      = 1.0f / 480.0f;
    world.max_substeps = 12;

    drop();

    /* Side-on camera: looking along -Z at the row of balls, lifted a touch and
     * tilted down so the floor and the full bounce arc are visible. */
    cam_pos   = v3(0.0f, 1.7f, -7.2f);
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.16f); /* pitch down */
}

/* The static floor lives in its own body so the solver collides every ball
 * against it. It is appended after the dynamic balls in the step call. */
static MoteBody s_floor;

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A))    drop();

    /* Auto re-drop on a ~3s cadence so the bounce is always visible. */
    s_redrop_t += dt;
    if (s_redrop_t >= 3.0f) drop();

    /* Assemble [balls..., floor] for the solver. The floor is a static PLANE
     * (inv_mass 0); restitution comes from each colliding ball. */
    static MoteBody sim[NBALL + 1];
    for (int i = 0; i < NBALL; i++) sim[i] = body[i];
    s_floor.shape    = MOTE_SHAPE_PLANE;
    s_floor.pos      = v3(0.0f, FLOOR_Y, 0.0f);
    s_floor.orient   = m3_identity();      /* r[1] = up = surface normal */
    s_floor.inv_mass = 0.0f;
    s_floor.radius   = 0.0f;
    sim[NBALL] = s_floor;

    mote->phys_step(&world, sim, NBALL + 1, dt);

    for (int i = 0; i < NBALL; i++) body[i] = sim[i];   /* copy back */

    /* ---- render ---- */
    mote->scene_begin(&cam_basis, 55.0f);

    MoteObject floor = { .pos = v3(0.0f, FLOOR_Y, 0.0f), .basis = m3_identity(),
                         .mesh = &k_floor_mesh };
    floor.pos = v3_sub(floor.pos, cam_pos);
    mote->scene_add_object_scaled(&floor, 4.0f);

    for (int i = 0; i < NBALL; i++) {
        Vec3 p = v3_sub(body[i].pos, cam_pos);
        mote->scene_add_sphere(p, body[i].radius, k_col[i]);
    }
}

static void g_overlay(uint16_t *fb) {
    char line[40], *p;
    p = ap_s(line, "MATERIALS - restitution"); *p = 0;
    mote->text(fb, line, 3, 3, MOTE_RGB565(255, 255, 255));

    /* Legend: value labels in each ball's colour, laid out as a row near the
     * top so they read as a key for the gradient below. */
    static const char *lbl[NBALL] = { "0.0", "0.3", "0.6", "0.85", "0.97" };
    int x = 3;
    for (int i = 0; i < NBALL; i++) {
        x = mote->text(fb, lbl[i], x, 13, k_col[i]);
        x = mote->text(fb, " ", x, 13, MOTE_RGB565(255, 255, 255));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
