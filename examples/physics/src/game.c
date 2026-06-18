/*
 * physics — bouncing, tumbling rigid bodies, proving the mote_phys solver
 * (gravity + impulse collisions + friction-driven spin) behind the ABI.
 *
 * Controls: A re-tosses the bodies; MENU exits.
 */
#include "mote_api.h"
#include "phys_meshes.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NBODY 8

static MoteWorld world;
static MoteBody  body[NBODY];
static uint32_t  rng = 1u;
static Vec3      cam_pos;
static Mat3      cam_basis;

static float frand(void) {       /* xorshift -> [-1,1) */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
}

static void toss(void) {
    for (int i = 0; i < NBODY; i++) {
        MoteBody *b = &body[i];
        b->radius = 0.30f;
        b->inv_mass = 1.0f / 0.3f;
        b->pos = v3(frand() * 1.2f, 1.2f + (float)i * 0.18f, frand() * 1.2f);
        b->vel = v3(frand() * 2.0f, frand() * 1.5f, frand() * 2.0f);
        b->w   = v3(frand() * 4.0f, frand() * 4.0f, frand() * 4.0f);
        b->orient = m3_identity();
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 12, 22));
    mote->scene_set_sun(v3(0.4f, 0.8f, -0.4f));

    mote->phys_world_defaults(&world);
    world.bmin = v3(-1.8f, -1.4f, -1.8f);
    world.bmax = v3( 1.8f,  3.0f,  1.8f);
    world.restitution = 0.55f;

    rng = (uint32_t)mote->micros() | 1u;
    toss();

    /* Camera: in front of the box, slightly above, tilted down to see the floor. */
    cam_pos = v3(0.0f, 0.6f, -5.2f);
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.16f);   /* pitch down a touch */
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    toss();

    mote->phys_step(&world, body, NBODY, dt);

    mote->scene_begin(&cam_basis, 55.0f);

    /* Floor. */
    MoteObject floor = { .pos = v3(0, world.bmin.y, 0), .basis = m3_identity(),
                         .mesh = &k_floor_mesh };
    floor.pos = v3_sub(floor.pos, cam_pos);
    mote->scene_add_object_scaled(&floor, 1.9f);

    /* Bodies as shaded sphere impostors — matches the sphere physics, rests
     * naturally on the floor. */
    static const uint16_t pal[4] = {
        MOTE_RGB565(235, 90, 90), MOTE_RGB565(90, 200, 110),
        MOTE_RGB565(90, 150, 240), MOTE_RGB565(235, 200, 80),
    };
    for (int i = 0; i < NBODY; i++)
        mote->scene_add_sphere(v3_sub(body[i].pos, cam_pos), body[i].radius, pal[i & 3]);
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
