/*
 * playground — a physics PLAYGROUND showcasing STATIC colliders + per-body
 * MATERIALS. A ground plane, two tilted brown ramps forming a shallow valley,
 * and low grey perimeter walls are all immovable (inv_mass = 0). A pool of
 * dynamic balls drops onto the ramps; the balls come in three MATERIALS whose
 * behaviour you can read off the colour:
 *
 *   - BOUNCY (red)   restitution 0.9   -> springs back up off the ramp/ground
 *   - ICE    (cyan)  friction 0.02     -> barely grips, slides far down-slope
 *   - NORMAL (gold)  world material     -> rolls and settles
 *
 * Statics + tilted-box colliders are built with mote_build.h (a tilt matrix is
 * the ramp's orient, used for BOTH render and collision). Balls re-drop on
 * their own when everything settles, so the scene stays lively.
 *
 * Style note — rendering uses scene_camera(): we pass the camera position once
 * per frame and add every object at WORLD coordinates (no hand-rolled v3_sub).
 *
 * Controls: A drop a ball · B reset all · (hold MENU 3s for the engine menu)
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* Body layout: statics first (fixed indices), then the dynamic ball pool. The
 * solver treats inv_mass==0 bodies as immovable colliders. */
#define MAT_BOUNCY  0
#define MAT_ICE     1
#define MAT_NORMAL  2

#define NBALL      10                /* dynamic ball pool */
#define I_GROUND   0
#define I_RAMP_L   1
#define I_RAMP_R   2
#define I_WALL0    3                 /* 4 perimeter walls */
#define NSTATIC    7                 /* ground + 2 ramps + 4 walls */
#define NTOTAL     (NSTATIC + NBALL)

static MoteWorld world;
static MoteBody  body[NTOTAL];
static int       ball_material[NBALL];  /* material of each pool ball */
static int       balls_live = 0;        /* how many balls have been spawned */
static int       next_slot = 0;         /* round-robin index into the pool */
static int       drop_count = 0;        /* lifetime drop counter (HUD) */
static float     settled_time = 0.0f;   /* seconds the scene has been at rest */

static Vec3  cam_pos;
static Mat3  cam_basis;

/* arena-built meshes (held from init) */
static const Mesh *m_ground, *m_ramp, *m_wall_x, *m_wall_z;

/* Ramp geometry. The two ramps form a shallow VALLEY: each tilts so its OUTER
 * edge is high and its inner (centre) edge is low, so a ball dropped on the
 * high edge rolls DOWN toward the middle where they meet. */
static const Vec3  RAMP_HALF = { 2.0f, 0.18f, 1.40f };
static const float RAMP_TILT = 0.44f;   /* ~25 degrees, about world Z */

/* ball colours by material (also used for the legend) */
static const uint16_t k_mat_col[3] = {
    MOTE_RGB565(238, 64, 64),    /* BOUNCY  red  */
    MOTE_RGB565(96, 222, 240),   /* ICE     cyan */
    MOTE_RGB565(244, 198, 64),   /* NORMAL  gold */
};

/* left ramp: high edge at -X -> tilt so +X goes down (negative about Z);
 * right ramp mirrors. */
static Mat3 ramp_orient(int left) {
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 2, left ? -RAMP_TILT : RAMP_TILT);
    return m;
}

static Mat3 wall_orient_z(void) {           /* rotate a +X wall to run along Z */
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 1, 1.5707963f);
    return m;
}

static void make_static(void) {
    /* Ground: infinite plane half-space, normal = orient.r[1] = +Y. */
    MoteBody *g = &body[I_GROUND];
    *g = (MoteBody){0};
    g->shape    = MOTE_SHAPE_PLANE;
    g->pos      = v3(0, 0, 0);
    g->orient   = m3_identity();
    g->inv_mass = 0.0f;

    /* Two tilted ramp slabs, one each side of centre. Centre raised so the low
     * (inner) edge clears the ground as the ~25deg tilt drops it. */
    for (int s = 0; s < 2; s++) {
        MoteBody *r = &body[I_RAMP_L + s];
        int left = (s == 0);
        *r = (MoteBody){0};
        r->shape    = MOTE_SHAPE_BOX;
        r->half     = RAMP_HALF;
        r->radius   = v3_len(RAMP_HALF);
        r->orient   = ramp_orient(left);
        r->inv_mass = 0.0f;
        r->friction = 0.55f;
        r->pos      = v3(left ? -1.9f : 1.9f, 1.02f, 0.0f);
    }

    /* Four low perimeter walls boxing in the play area (~+-4 m). */
    const Vec3 wall_half_x = { 4.0f, 0.4f, 0.18f };   /* runs along X */
    struct { Vec3 pos; int along_x; } w[4] = {
        { v3(0.0f,  0.4f, -4.0f), 1 },   /* far  */
        { v3(0.0f,  0.4f,  4.0f), 1 },   /* near */
        { v3(-4.0f, 0.4f,  0.0f), 0 },   /* left */
        { v3( 4.0f, 0.4f,  0.0f), 0 },   /* right */
    };
    Mat3 zrot = wall_orient_z();
    for (int i = 0; i < 4; i++) {
        MoteBody *b = &body[I_WALL0 + i];
        *b = (MoteBody){0};
        b->shape    = MOTE_SHAPE_BOX;
        b->half     = w[i].along_x ? wall_half_x
                                   : v3(wall_half_x.z, wall_half_x.y, wall_half_x.x);
        b->radius   = v3_len(b->half);
        b->orient   = w[i].along_x ? m3_identity() : zrot;
        b->inv_mass = 0.0f;
        b->friction = 0.6f;
        b->pos      = w[i].pos;
    }
}

/* Spawn one ball into pool slot `slot` on the high edge of a ramp, with a small
 * push down-slope. Cycles materials so the mix is always visible. */
static void spawn_ball(int slot) {
    MoteBody *b = &body[NSTATIC + slot];
    int mat = slot % 3;
    ball_material[slot] = mat;

    *b = (MoteBody){0};
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = 0.27f;
    b->inv_mass = 1.0f / 0.3f;
    b->orient   = m3_identity();

    switch (mat) {
        case MAT_BOUNCY:  b->restitution = 0.9f;  b->friction = 0.30f; break;
        case MAT_ICE:     b->restitution = 0.05f; b->friction = 0.02f; break;
        default:          b->restitution = 0.25f; b->friction = 0.5f;  break; /* world-ish */
    }

    /* Alternate which ramp we drop from: left ramp high edge at -X, right at +X.
     * The ball then rolls down toward the centre valley. */
    int left = (slot & 1) == 0;
    float hi_x = left ? -3.3f : 3.3f;
    b->pos = v3(hi_x + mote_randf(-1.0f, 1.0f) * 0.25f, 3.1f, mote_randf(-1.0f, 1.0f) * 0.8f);
    b->vel = v3(left ? 0.8f : -0.8f, 0.0f, 0.0f);
    b->_reserved[0] = 0;                          /* wake */

    if (slot >= balls_live) balls_live = slot + 1;
    drop_count++;
}

static void park_ball(int slot) {
    MoteBody *b = &body[NSTATIC + slot];
    *b = (MoteBody){0};
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = 0.27f;
    b->inv_mass = 0.0f;                /* solver ignores it */
    b->orient   = m3_identity();
    b->pos      = v3(0.0f, -50.0f, 0.0f);
}

static void reset_all(void) {
    for (int i = 0; i < NBALL; i++) park_ball(i);
    balls_live = 0;
    next_slot = 0;
    settled_time = 0.0f;
}

/* True once every live ball is moving slowly (the pile has settled). */
static int scene_at_rest(void) {
    int any = 0;
    for (int i = 0; i < NBALL; i++) {
        MoteBody *b = &body[NSTATIC + i];
        if (b->inv_mass == 0.0f) continue;
        any = 1;
        if (v3_len(b->vel) > 0.35f) return 0;
    }
    return any;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120, 165, 210));   /* sky */
    mote->scene_set_sun(v3_norm(v3(0.45f, 0.85f, -0.3f)));

    mote->phys_world_defaults(&world);
    world.walls        = 0;                  /* NO auto box; we build statics */
    world.gravity      = v3(0.0f, -9.8f, 0.0f);
    world.substep      = 1.0f / 240.0f;
    world.max_substeps = 8;
    world.restitution  = 0.25f;
    world.friction     = 0.5f;

    /* arena meshes (half-extents). Ramp/wall colliders reuse these dims. */
    m_ground = mote_mesh_box(mote, 6.0f, 0.10f, 6.0f, MOTE_RGB565(70, 96, 64));
    m_ramp   = mote_mesh_box(mote, RAMP_HALF.x, RAMP_HALF.y, RAMP_HALF.z, MOTE_RGB565(156, 100, 56));
    m_wall_x = mote_mesh_box(mote, 4.0f, 0.4f, 0.18f, MOTE_RGB565(122, 126, 134));
    m_wall_z = mote_mesh_box(mote, 0.18f, 0.4f, 4.0f, MOTE_RGB565(122, 126, 134));

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    make_static();
    reset_all();

    /* Seed a full mix so material variety shows immediately. */
    for (int i = 0; i < 6; i++) spawn_ball(i);
    next_slot = 6 % NBALL;

    /* Camera: off to the side and above, framing the whole valley. */
    cam_pos   = v3(0.5f, 4.0f, -7.8f);
    cam_basis = mote_camera_look(cam_pos, v3(0.0f, 0.7f, 0.0f));
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_B)) reset_all();
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        spawn_ball(next_slot);
        next_slot = (next_slot + 1) % NBALL;
    }

    mote->phys_step(&world, body, NTOTAL, dt);

    /* Keep it lively: once everything has been at rest for a moment, drop a
     * fresh ball; if the pool is full, clear and start a new wave. */
    if (scene_at_rest()) {
        settled_time += dt;
        if (settled_time > 1.4f) {
            settled_time = 0.0f;
            if (balls_live >= NBALL) reset_all();
            spawn_ball(next_slot);
            next_slot = (next_slot + 1) % NBALL;
        }
    } else {
        settled_time = 0.0f;
    }

    /* ---- render (world coordinates; scene_camera subtracts the camera for us) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 56.0f);

    /* Ground slab (top face sits at y=0). */
    mote_draw(mote, m_ground, v3(0.0f, -0.10f, 0.0f));

    /* Ramps — tilted boxes; the body's orient IS the tilt used for collision. */
    for (int s = 0; s < 2; s++) {
        MoteBody *r = &body[I_RAMP_L + s];
        mote_draw_ex(mote, m_ramp, r->pos, r->orient, 1.0f);
    }

    /* Walls. */
    for (int i = 0; i < 4; i++) {
        MoteBody *b = &body[I_WALL0 + i];
        const Mesh *wm = (i < 2) ? m_wall_x : m_wall_z;
        mote_draw_ex(mote, wm, b->pos, b->orient, 1.0f);
    }

    /* Balls: shaded impostor spheres coloured by material. */
    for (int i = 0; i < NBALL; i++) {
        MoteBody *b = &body[NSTATIC + i];
        if (b->inv_mass == 0.0f) continue;       /* parked / not spawned */
        mote->scene_add_sphere(b->pos, b->radius, k_mat_col[ball_material[i]]);
    }
}

static void g_overlay(uint16_t *fb) {
    /* title panel */
    mote_ui_panel(fb, 1, 1, 108, 11, MOTE_RGB565(16, 20, 30), MOTE_RGB565(70, 90, 130));
    int x = mote->text(fb, "PLAYGROUND", 4, 3, MOTE_RGB565(235, 240, 250));
    char num[12]; mote_itoa(drop_count, num);
    x = mote->text(fb, "  x", x, 3, MOTE_RGB565(150, 170, 200));
    mote->text(fb, num, x, 3, MOTE_RGB565(150, 220, 255));

    /* material legend with colour swatches */
    static const char *k_name[3] = { "BOUNCY", "ICE", "NORMAL" };
    int ly = 92;
    for (int m = 0; m < 3; m++) {
        mote_ui_rect(fb, 3, ly + 1, 6, 6, k_mat_col[m]);
        mote->text(fb, k_name[m], 12, ly, k_mat_col[m]);
        ly += 9;
    }

    mote->text(fb, "A DROP  B RESET", 3, 120, MOTE_RGB565(160, 180, 210));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 220, .max_spheres = NBALL, .max_bodies = NTOTAL,
                .max_contacts = 96, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
