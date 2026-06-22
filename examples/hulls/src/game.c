/*
 * hulls — convex-HULL colliders: dice and gems that collide and STACK stably.
 *
 * Each body is MOTE_SHAPE_HULL (a real convex polyhedron with faces + edges);
 * collision uses SAT + face clipping for multi-point manifolds, so they rest on
 * faces and pile up cleanly instead of jittering. Contained by a chamber of
 * static planes (planes give hulls stable multi-point support). The pile is
 * kept lively: once everything settles it auto-tosses again so there's always
 * a tumble to watch.
 *
 * Controls: A = toss everything up · B = reset · LEFT/RIGHT orbit the camera
 *
 * Style notes — this example uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so bodies are drawn at WORLD
 *     coordinates and the engine subtracts the camera (no v3_sub anywhere).
 *   · mote_draw / mote_draw_ex build the MoteObject for us.
 *   · mote_randf replaces the hand-rolled xorshift RNG.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "hull_solids.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define ARENA 1.25f
#define NPLANE 5
#define NDYN   9
#define NBODY  (NPLANE + NDYN)

static MoteWorld world;
static MoteBody  body[NBODY];
static const Mesh *dyn_mesh[NDYN];
static float       dyn_scale[NDYN];
static const Mesh *floor_mesh;

/* Build an orientation basis whose +Y points along the given up vector. */
static Mat3 orient_up(Vec3 up) {
    Mat3 m;
    m.r[1] = v3_norm(up);

    Vec3 t = (fabsf(m.r[1].y) < 0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
    m.r[0] = v3_norm(v3_cross(t, m.r[1]));
    m.r[2] = v3_cross(m.r[0], m.r[1]);

    return m;
}

/* Fling all the dynamic bodies upward. When `reset`, also re-stack them. */
static void toss(int reset) {
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];

        b->vel = v3(mote_randf(-1, 1) * 0.7f, 1.8f + 0.5f * mote_randf(-1, 1), mote_randf(-1, 1) * 0.7f);
        b->w   = v3(mote_randf(-1, 1) * 3.0f, mote_randf(-1, 1) * 3.0f, mote_randf(-1, 1) * 3.0f);
        b->_reserved[0] = 0;                 /* wake the sleeper */

        if (reset) {
            b->pos = v3(mote_randf(-1, 1) * 0.6f, 0.9f + k * 0.42f, mote_randf(-1, 1) * 0.6f);
            b->orient = m3_identity();
            m3_rotate_local(&b->orient, 0, mote_randf(-1, 1) * 3.0f);
            m3_rotate_local(&b->orient, 2, mote_randf(-1, 1) * 3.0f);
        }
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 14, 24));
    mote->scene_set_sun(v3_norm(v3(0.35f, 0.92f, -0.2f)));

    /* A low slab under the open chamber so the pile reads against a surface. */
    floor_mesh = mote_mesh_box(mote, ARENA, 0.06f, ARENA, MOTE_RGB565(40, 58, 80));

    mote->phys_world_defaults(&world);
    world.walls = 0;
    world.gravity = v3(0, -9.8f, 0);
    world.restitution = 0.18f;
    world.friction = 0.5f;
    world.substep = 1.0f / 300.0f;
    world.max_substeps = 10;

    /* Five static planes form an open-top chamber. */
    Vec3 nrm[NPLANE] = { v3(0,1,0), v3(1,0,0), v3(-1,0,0), v3(0,0,1), v3(0,0,-1) };
    Vec3 pt [NPLANE] = { v3(0,0,0), v3(-ARENA,0,0), v3(ARENA,0,0), v3(0,0,-ARENA), v3(0,0,ARENA) };
    for (int i = 0; i < NPLANE; i++) {
        body[i].shape = MOTE_SHAPE_PLANE;
        body[i].inv_mass = 0;
        body[i].pos = pt[i];
        body[i].orient = orient_up(nrm[i]);
        body[i].friction = 0.6f;
        body[i].restitution = 0.2f;
    }

    /* Dynamic bodies alternate octahedral gems (odd) and cubic dice (even). */
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        b->shape = MOTE_SHAPE_HULL;
        b->inv_mass = 1.0f / 0.3f;
        b->restitution = 0.2f;
        b->friction = 0.55f;

        if (k & 1) {
            b->shape_data = &oct_hull;  b->radius = oct_hull.bound_r;  dyn_mesh[k] = &oct_rmesh;  dyn_scale[k] = OCT_S;
        } else {
            b->shape_data = &cube_hull; b->radius = cube_hull.bound_r; dyn_mesh[k] = &cube_rmesh; dyn_scale[k] = CUBE_S;
        }
    }

    toss(1);
}

static Vec3 cam_pos;
static Mat3 cam_basis;
static float orbit = 0.6f;     /* azimuth (rad) */
static int   settle_frames = 0;       /* frames the pile has been near-still */

/* How fast the liveliest body is moving — used to detect a settled pile. */
static float max_motion(void) {
    float m = 0;
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        float v = v3_len(b->vel) + v3_len(b->w) * 0.2f;
        if (v > m) m = v;
    }
    return m;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    int auto_orbit = 1;
    if (mote_just_pressed(in, MOTE_BTN_B))   toss(1);
    if (mote_just_pressed(in, MOTE_BTN_A))   toss(0);
    if (mote_pressed(in, MOTE_BTN_LEFT))  { orbit -= 1.2f * dt; auto_orbit = 0; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { orbit += 1.2f * dt; auto_orbit = 0; }
    if (auto_orbit) orbit += 0.18f * dt;     /* slow drift so the pile is always shown in the round */

    mote->phys_step(&world, body, NBODY, dt);

    /* Keep it lively: when the whole pile has been calm for a moment, re-toss. */
    if (max_motion() < 0.12f) {
        if (++settle_frames > 70) { toss(0); settle_frames = 0; }
    } else {
        settle_frames = 0;
    }

    float r = 3.5f;
    cam_pos = v3(sinf(orbit) * r, 2.15f, -cosf(orbit) * r);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0.25f, 0));

    /* ---- render (world coordinates; scene_camera subtracts the camera) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 56.0f);

    /* Floor slab (planes are invisible). */
    mote_draw(mote, floor_mesh, v3(0, -0.06f, 0));

    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        mote_draw_ex(mote, dyn_mesh[k], b->pos, b->orient, dyn_scale[k]);
    }
}

static void g_overlay(uint16_t *fb) {
    /* Top panel with title + dice/gem split. */
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 13, MOTE_RGB565(18, 24, 38), MOTE_RGB565(70, 90, 130));
    mote->text(fb, "HULLS", 3, 3, MOTE_RGB565(245, 240, 220));

    char line[16];
    int q = 0;
    int dice = (NDYN + 1) / 2;
    int gems = NDYN / 2;  /* even idx = die, odd = gem */

    q += mote_itoa(dice, line + q);
    line[q++] = 'D';
    line[q++] = ' ';
    q += mote_itoa(gems, line + q);
    line[q++] = 'G';
    line[q] = 0;

    int rx = MOTE_FB_W - 6 * q - 4;
    mote->text(fb, line, rx, 3, MOTE_RGB565(140, 220, 245));

    /* Calm/active meter: full when the pile is settled (about to re-toss). */
    float calm = (float)settle_frames / 70.0f;
    if (calm > 1) calm = 1;
    mote_ui_bar(fb, 2, 15, MOTE_FB_W - 4, 3, calm,
                MOTE_RGB565(120, 200, 235), MOTE_RGB565(28, 34, 48));

    mote->text(fb, "A TOSS  B RESET", 6, 118, MOTE_RGB565(160, 172, 192));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = NDYN * 12 + 12,  /* 9 hulls (<=12 tris each) + floor slab (12) */
                .max_bodies = NBODY, .max_contacts = 128, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
