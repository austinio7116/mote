/*
 * materials — a physics-materials playground. A mix of bouncy balls and heavy
 * boxes pours into a bin and piles up: the lively (high-restitution) balls bounce
 * and scatter, the boxes stack and settle. Watch the materials interact, then A
 * to pour a fresh batch or B to SHAKE the bin and jumble everything.
 *
 * Controls: A re-drop · B shake · (hold MENU 3s for the engine menu)
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NOBJ 12

static MoteWorld world;
static MoteBody  body[NOBJ];
static const Mesh *m_box[3], *m_floor, *m_back, *m_rail;
static Vec3  cam_pos;
static Mat3  cam_basis;

/* fixed per-object identity (set once); only the transform re-rolls on a drop */
static int      o_box[NOBJ];      /* 0 = ball, else box-size index 1..3 */
static float    o_radius[NOBJ];   /* ball radius */
static uint16_t o_col[NOBJ];

/* the three box shapes, by size index (o_box - 1) */
static const Vec3 k_boxhalf[3] = { {0.30f,0.30f,0.30f}, {0.42f,0.22f,0.30f}, {0.22f,0.46f,0.22f} };

/* rotation about the Y axis (used to give each dropped box a random heading) */
static Mat3 roty(float a){
    float c = cosf(a), s = sinf(a);
    Mat3 m;
    m.r[0] = v3(c, 0, -s);
    m.r[1] = v3(0, 1, 0);
    m.r[2] = v3(s, 0, c);
    return m;
}

/* pour a fresh batch: re-roll each object's transform high in the bin */
static void drop_batch(void){
    for(int i = 0; i < NOBJ; i++){
        MoteBody *b = &body[i];
        int isbox = o_box[i];

        b->pos    = v3(mote_randf(-2.0f, 6.0f), mote_randf(3.8f, 8.6f), mote_randf(-0.4f, 2.8f));
        b->vel    = v3(0, 0, 0);
        b->w      = v3(mote_randf(-2, 6), mote_randf(-2, 6), mote_randf(-2, 6));
        b->orient = roty(mote_randf(0, 6.28f * 2.0f));

        if(isbox){
            b->shape    = MOTE_SHAPE_BOX;
            b->half     = k_boxhalf[isbox-1];
            b->radius   = v3_len(b->half);
            b->inv_mass = 1.0f / 1.4f;
            b->restitution = 0.12f;
            b->friction = 0.7f;
        } else {
            b->shape    = MOTE_SHAPE_SPHERE;
            b->radius   = o_radius[i];
            b->inv_mass = 1.0f / 0.5f;
            b->restitution = mote_randf(0.4f, 1.4f);
            b->friction = 0.35f;
        }
        b->_reserved[0] = 0;
    }
}

/* kick everything upward with random spin */
static void shake(void){
    for(int i = 0; i < NOBJ; i++){
        body[i].vel = v3(mote_randf(-3, 9), mote_randf(4, 10), mote_randf(-3, 9));
        body[i].w   = v3(mote_randf(-5, 15), mote_randf(-5, 15), mote_randf(-5, 15));
        body[i]._reserved[0] = 0;
    }
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120,150,200));
    mote->scene_set_sun(v3_norm(v3(0.35f,0.9f,-0.4f)));

    mote->phys_world_defaults(&world);
    world.gravity = v3(0,-9.8f,0);
    world.walls = 1;
    world.bmin = v3(-2.6f,0.0f,-1.2f);
    world.bmax = v3(2.6f,9.0f,1.6f);             /* the bin */
    world.restitution = 0.3f;
    world.friction = 0.5f;
    world.substep = 1.0f/480.0f;
    world.max_substeps = 10;
    mote_rand_seed((uint32_t)mote->micros());

    m_box[0] = mote_mesh_box(mote, 0.30f,0.30f,0.30f, MOTE_RGB565(90,150,235));
    m_box[1] = mote_mesh_box(mote, 0.42f,0.22f,0.30f, MOTE_RGB565(120,200,150));
    m_box[2] = mote_mesh_box(mote, 0.22f,0.46f,0.22f, MOTE_RGB565(180,130,225));
    m_floor  = mote_mesh_box(mote, 2.6f,0.12f,1.4f,   MOTE_RGB565(70,84,70));
    m_back   = mote_mesh_box(mote, 2.6f,1.1f,0.08f,   MOTE_RGB565(54,66,84));
    m_rail   = mote_mesh_box(mote, 0.08f,0.5f,1.4f,   MOTE_RGB565(60,72,92));

    /* fixed identities: alternate balls (warm) and boxes (cool) */
    for(int i = 0; i < NOBJ; i++){
        if(i & 1){
            o_box[i] = 1 + (i/2) % 3;
            o_col[i] = 0;
        } else {
            o_box[i] = 0;
            o_radius[i] = mote_randf(0.28f, 0.52f);
            o_col[i] = (i%4==0) ? MOTE_RGB565(235,90,80)
                     : (i%4==2) ? MOTE_RGB565(240,170,60)
                                : MOTE_RGB565(245,210,90);
        }
    }
    drop_batch();

    cam_pos   = v3(0.0f,2.6f,-6.4f);
    cam_basis = mote_camera_look(cam_pos, v3(0,0.9f,0));
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if(mote_just_pressed(in,MOTE_BTN_A)) drop_batch();
    if(mote_just_pressed(in,MOTE_BTN_B)) shake();

    mote->phys_step(&world, body, NOBJ, dt);

    /* render in world coordinates; scene_camera subtracts the camera for us */
    mote->scene_camera(&cam_basis, cam_pos, 56.0f);

    /* the bin: floor + back wall + two side rails (visual; physics uses world.walls) */
    mote_draw(mote, m_floor, v3(0,-0.12f,0));
    mote_draw(mote, m_back,  v3(0,1.0f,1.5f));
    for(int s = -1; s <= 1; s += 2){
        mote_draw(mote, m_rail, v3(s*2.58f, 0.4f, 0));
    }

    /* the objects: boxes get their orientation, balls draw as impostor spheres */
    for(int i = 0; i < NOBJ; i++){
        if(o_box[i]){
            mote_draw_ex(mote, m_box[o_box[i]-1], body[i].pos, body[i].orient, 1.0f);
        } else {
            mote->scene_add_sphere(body[i].pos, body[i].radius, o_col[i]);
        }
    }
}

static void g_overlay(uint16_t *fb){
    mote_ui_panel(fb,1,1,86,11,MOTE_RGB565(16,20,30),MOTE_RGB565(70,90,130));
    mote->text(fb,"MATERIALS  bin",4,3,MOTE_RGB565(235,240,250));
    mote->text(fb,"A POUR   B SHAKE",3,118,MOTE_RGB565(160,180,210));
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=420, .max_spheres=24, .max_bodies=NOBJ, .max_contacts=160, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
