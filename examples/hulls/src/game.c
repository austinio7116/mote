/*
 * hulls — convex-HULL colliders: dice and gems that collide and STACK stably.
 *
 * Each body is MOTE_SHAPE_HULL (a real convex polyhedron with faces + edges);
 * collision uses SAT + face clipping for multi-point manifolds, so they rest on
 * faces and pile up cleanly instead of jittering. Contained by a chamber of
 * static planes (planes give hulls stable multi-point support).
 *
 * Controls: A = toss everything up · B = reset · LEFT/RIGHT orbit · MENU exit
 */
#include "mote_api.h"
#include "hull_solids.h"
#include <math.h>

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

static uint32_t rng = 99991u;
static float frand(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng&0xFFFF)/32768.0f - 1.0f; }

static Mat3 orient_up(Vec3 up) {
    Mat3 m; m.r[1] = v3_norm(up);
    Vec3 t = (fabsf(m.r[1].y) < 0.9f) ? v3(0,1,0) : v3(1,0,0);
    m.r[0] = v3_norm(v3_cross(t, m.r[1]));
    m.r[2] = v3_cross(m.r[0], m.r[1]);
    return m;
}

static void toss(int reset) {
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        b->vel = v3(frand()*0.7f, 1.8f + 0.5f*frand(), frand()*0.7f);
        b->w   = v3(frand()*3.0f, frand()*3.0f, frand()*3.0f);
        if (reset) {
            b->pos = v3(frand()*0.6f, 0.9f + k*0.42f, frand()*0.6f);
            b->orient = m3_identity();
            m3_rotate_local(&b->orient, 0, frand()*3.0f);
            m3_rotate_local(&b->orient, 2, frand()*3.0f);
            b->_reserved[0] = 0;
        }
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 16, 26));
    mote->scene_set_sun(v3(0.3f, 0.9f, 0.25f));

    mote->phys_world_defaults(&world);
    world.walls = 0;
    world.gravity = v3(0, -9.8f, 0);
    world.restitution = 0.18f; world.friction = 0.5f;
    world.substep = 1.0f/300.0f; world.max_substeps = 10;

    Vec3 nrm[NPLANE] = { v3(0,1,0), v3(1,0,0), v3(-1,0,0), v3(0,0,1), v3(0,0,-1) };
    Vec3 pt [NPLANE] = { v3(0,0,0), v3(-ARENA,0,0), v3(ARENA,0,0), v3(0,0,-ARENA), v3(0,0,ARENA) };
    for (int i = 0; i < NPLANE; i++) {
        body[i].shape = MOTE_SHAPE_PLANE; body[i].inv_mass = 0;
        body[i].pos = pt[i]; body[i].orient = orient_up(nrm[i]);
        body[i].friction = 0.6f; body[i].restitution = 0.2f;
    }
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        b->shape = MOTE_SHAPE_HULL; b->inv_mass = 1.0f/0.3f; b->restitution = 0.2f;
        if (k & 1) { b->shape_data = &oct_hull;  b->radius = oct_hull.bound_r;  dyn_mesh[k] = &oct_rmesh;  dyn_scale[k] = OCT_S; }
        else       { b->shape_data = &cube_hull; b->radius = cube_hull.bound_r; dyn_mesh[k] = &cube_rmesh; dyn_scale[k] = CUBE_S; }
    }
    toss(1);
}

static Vec3 cam_pos; static Mat3 cam_basis; static float s_orbit = 0.6f;

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_B))    toss(1);
    if (mote_just_pressed(in, MOTE_BTN_A))    toss(0);
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_orbit -= 1.0f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_orbit += 1.0f*dt;

    mote->phys_step(&world, body, NBODY, dt);

    float r = 3.4f;
    cam_pos = v3(sinf(s_orbit)*r, 2.0f, -cosf(s_orbit)*r);
    Vec3 fwd = v3_norm(v3_sub(v3(0,0.15f,0), cam_pos));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    cam_basis.r[0] = right; cam_basis.r[1] = v3_cross(fwd, right); cam_basis.r[2] = fwd;

    mote->scene_begin(&cam_basis, 56.0f);
    /* floor quad (planes are invisible) */
    static const MeshVert fv[4] = {{-127,0,-127},{127,0,-127},{127,0,127},{-127,0,127}};
    static const MeshFace ff[2] = {{0,3,2,0,127,0,MOTE_RGB565(38,56,76)},{0,2,1,0,127,0,MOTE_RGB565(38,56,76)}};
    static const Mesh floor_mesh = { fv, ff, 4, 2, ARENA, ARENA*1.5f, 0 };
    MoteObject fo = { .pos = v3_sub(v3(0,0,0), cam_pos), .basis = m3_identity(), .mesh = &floor_mesh };
    mote->scene_add_object(&fo);

    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        MoteObject o = { .pos = v3_sub(b->pos, cam_pos), .basis = b->orient, .mesh = dyn_mesh[k] };
        mote->scene_add_object_scaled(&o, dyn_scale[k]);
    }
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "HULLS: DICE+GEMS", 3, 3, MOTE_RGB565(215,225,240));
    mote->text(fb, "A TOSS  B RESET", 3, 118, MOTE_RGB565(150,160,180));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
