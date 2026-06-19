/*
 * hulls — imported 3D models colliding via CONVEX HULL + CAPSULE colliders.
 *
 * Imports four ThumbyElite ship models (baked OBJ -> mesh), renders each as the
 * full mesh but COLLIDES it as the convex hull of its vertices (MOTE_SHAPE_HULL,
 * resolved by GJK/EPA) — the standard "render mesh != collision proxy" approach.
 * Also drops a CAPSULE. They tumble, bounce off each other and the walls, and
 * settle on the floor.
 *
 * NOTE on scope: GJK/EPA gives single-point hull-hull contacts — great for
 * tumbling/colliding, not for tall rigid stacks (multi-point hull manifolds are
 * a future addition). Hull/capsule rest stably on PLANES (multi-point), so the
 * arena is built from static planes, not the box-wall fallback (which only
 * knows sphere/box).
 *
 * Controls: A = toss everything up again · B = reset · LEFT/RIGHT orbit · MENU exit
 */
#include "mote_api.h"
#include "canister.h"
#include "beacon.h"
#include "viper.h"
#include "courier.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define DSCALE 0.16f          /* shrink the (4.5 m) ships to ~0.8 m props */
#define ARENA  1.35f          /* chamber half-width (x,z) */

/* per-model: float hull verts + the MoteHull + render mesh */
typedef struct { Vec3 v[64]; MoteHull hull; const Mesh *mesh; } Model;
static Model s_model[4];
static uint32_t rng = 2463534242u;
static float frand(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng&0xFFFF)/32768.0f - 1.0f; }

static void build_model(Model *m, const Mesh *mesh) {
    m->mesh = mesh;
    float s = mesh->scale / 127.0f * DSCALE;
    int n = mesh->nverts > 64 ? 64 : mesh->nverts;
    for (int i = 0; i < n; i++)
        m->v[i] = v3(mesh->verts[i].x * s, mesh->verts[i].y * s, mesh->verts[i].z * s);
    m->hull.verts = m->v; m->hull.nverts = n; m->hull.bound_r = mesh->bound_r * DSCALE;
}

/* a basis whose UP axis (r[1]) is `up` — for plane normals. */
static Mat3 orient_up(Vec3 up) {
    Mat3 m; m.r[1] = v3_norm(up);
    Vec3 t = (fabsf(m.r[1].y) < 0.9f) ? v3(0,1,0) : v3(1,0,0);
    m.r[0] = v3_norm(v3_cross(t, m.r[1]));
    m.r[2] = v3_cross(m.r[0], m.r[1]);
    return m;
}

#define NPLANE 5
#define NDYN   6
#define NBODY  (NPLANE + NDYN)
static MoteWorld world;
static MoteBody  body[NBODY];      /* [0..4] static planes, [5..] dynamic */
static const Mesh *dyn_mesh[NDYN]; /* render mesh for hull bodies, NULL for capsule */
static uint16_t   dyn_col[NDYN];

static Vec3 cam_pos; static Mat3 cam_basis; static float s_orbit = 0.7f;

static void toss(int reset) {
    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        b->vel = v3(frand()*0.8f, 2.2f + 0.6f*frand(), frand()*0.8f);
        b->w   = v3(frand()*4.0f, frand()*4.0f, frand()*4.0f);
        if (reset) {
            b->pos = v3(frand()*0.7f, 0.9f + k*0.45f, frand()*0.7f);
            b->orient = m3_identity();
            m3_rotate_local(&b->orient, 0, frand()*3.0f);
            m3_rotate_local(&b->orient, 2, frand()*3.0f);
            b->_reserved[0] = 0;
        }
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 14, 22));
    mote->scene_set_sun(v3(0.3f, 0.9f, 0.25f));

    build_model(&s_model[0], &viper_mesh);
    build_model(&s_model[1], &courier_mesh);
    build_model(&s_model[2], &canister_mesh);
    build_model(&s_model[3], &beacon_mesh);

    mote->phys_world_defaults(&world);
    world.walls = 0;                 /* arena = static planes (hull/capsule safe) */
    world.gravity = v3(0, -9.8f, 0);
    world.restitution = 0.35f; world.friction = 0.4f;
    world.substep = 1.0f/300.0f; world.max_substeps = 10;

    /* static planes: floor + 4 inward walls */
    Vec3 nrm[NPLANE] = { v3(0,1,0), v3(1,0,0), v3(-1,0,0), v3(0,0,1), v3(0,0,-1) };
    Vec3 pt [NPLANE] = { v3(0,0,0), v3(-ARENA,0,0), v3(ARENA,0,0), v3(0,0,-ARENA), v3(0,0,ARENA) };
    for (int i = 0; i < NPLANE; i++) {
        body[i].shape = MOTE_SHAPE_PLANE; body[i].inv_mass = 0.0f;
        body[i].pos = pt[i]; body[i].orient = orient_up(nrm[i]);
        body[i].friction = 0.5f; body[i].restitution = 0.3f;
    }
    /* dynamic: 4 ship hulls + a capsule + a bouncy sphere */
    static const uint16_t col[NDYN] = {
        MOTE_RGB565(200,210,235), MOTE_RGB565(220,160,80), MOTE_RGB565(140,200,150),
        MOTE_RGB565(235,120,120), MOTE_RGB565(170,150,235), MOTE_RGB565(235,210,90),
    };
    for (int k = 0; k < NDYN; k++) { dyn_col[k] = col[k]; dyn_mesh[k] = 0; }
    for (int k = 0; k < 4; k++) {
        MoteBody *b = &body[NPLANE + k];
        b->shape = MOTE_SHAPE_HULL; b->shape_data = &s_model[k].hull;
        b->radius = s_model[k].hull.bound_r; b->inv_mass = 1.0f/0.4f;
        b->restitution = 0.4f;
        dyn_mesh[k] = s_model[k].mesh;
    }
    /* [4] capsule */
    { MoteBody *b = &body[NPLANE + 4];
      b->shape = MOTE_SHAPE_CAPSULE; b->radius = 0.14f; b->half = v3(0, 0.22f, 0);
      b->inv_mass = 1.0f/0.4f; b->restitution = 0.3f; }
    /* [5] bouncy sphere */
    { MoteBody *b = &body[NPLANE + 5];
      b->shape = MOTE_SHAPE_SPHERE; b->radius = 0.18f; b->inv_mass = 1.0f/0.3f; b->restitution = 0.75f; }

    toss(1);
    cam_pos = v3(0, 1.4f, -2.6f); cam_basis = m3_identity();
}

static void render_capsule(const MoteBody *b, uint16_t col) {
    Vec3 ax = b->orient.r[1];
    for (int s = -1; s <= 1; s++) {
        Vec3 p = v3_add(b->pos, v3_scale(ax, (float)s * b->half.y));
        mote->scene_add_sphere(v3_sub(p, cam_pos), b->radius, col);
    }
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_B))    toss(1);
    if (mote_just_pressed(in, MOTE_BTN_A))    toss(0);
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_orbit -= 1.0f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_orbit += 1.0f * dt;

    mote->phys_step(&world, body, NBODY, dt);

    /* orbit camera around the chamber centre */
    float r = 2.7f;
    cam_pos = v3(sinf(s_orbit)*r, 1.55f, -cosf(s_orbit)*r);
    Vec3 target = v3(0, 0.35f, 0);
    Vec3 fwd = v3_norm(v3_sub(target, cam_pos));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    cam_basis.r[0] = right; cam_basis.r[1] = v3_cross(fwd, right); cam_basis.r[2] = fwd;

    mote->scene_begin(&cam_basis, 56.0f);
    /* floor quad (the planes are invisible) */
    static const MeshVert fv[4] = {{-127,0,-127},{127,0,-127},{127,0,127},{-127,0,127}};
    static const MeshFace ff[2] = {
        {0,3,2, 0,127,0, MOTE_RGB565(40,60,80)}, {0,2,1, 0,127,0, MOTE_RGB565(40,60,80)},
    };
    static const Mesh floor_mesh = { fv, ff, 4, 2, ARENA, ARENA*1.5f, 0 };
    MoteObject fo = { .pos = v3_sub(v3(0,0,0), cam_pos), .basis = m3_identity(), .mesh = &floor_mesh };
    mote->scene_add_object(&fo);

    for (int k = 0; k < NDYN; k++) {
        MoteBody *b = &body[NPLANE + k];
        if (b->shape == MOTE_SHAPE_HULL) {
            MoteObject o = { .pos = v3_sub(b->pos, cam_pos), .basis = b->orient, .mesh = dyn_mesh[k] };
            mote->scene_add_object_scaled(&o, DSCALE);
        } else if (b->shape == MOTE_SHAPE_CAPSULE) {
            render_capsule(b, dyn_col[k]);
        } else {
            mote->scene_add_sphere(v3_sub(b->pos, cam_pos), b->radius, dyn_col[k]);
        }
    }
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "HULLS + CAPSULE", 3, 3, MOTE_RGB565(210,220,235));
    mote->text(fb, "A TOSS  B RESET", 3, 118, MOTE_RGB565(150,160,180));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
