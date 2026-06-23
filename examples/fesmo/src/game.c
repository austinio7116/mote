/* fesmo — a 3D physics Mote game: a pile of boxes tumbling in a walled pit.
 * A re-tosses them. The pools below cover the bodies + their contacts. */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_phys.h"

#include "icon.h"
MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NB 16
static MoteWorld world;
static MoteBody body[NB];
static const Mesh *s_box, *s_floor;
static float s_t;

static void toss(void) {
    for (int i = 0; i < NB; i++) {
        MoteBody *b = &body[i]; *b = (MoteBody){0};
        b->shape = MOTE_SHAPE_BOX; b->half = v3(0.4f, 0.4f, 0.4f); b->radius = v3_len(b->half);
        b->pos = v3(mote_randf(-1.0f, 1.0f), 1.5f + i * 0.7f, mote_randf(-1.0f, 1.0f));
        b->orient = m3_identity(); b->inv_mass = 1.0f / 0.6f; b->friction = 0.6f; b->restitution = 0.1f;
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 14, 28));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.8f, -0.5f)));
    s_box   = mote_mesh_box(mote, 0.4f, 0.4f, 0.4f, MOTE_RGB565(210, 150, 90));
    s_floor = mote_mesh_box(mote, 1.7f, 0.1f, 1.7f, MOTE_RGB565(60, 72, 92));
    mote->phys_world_defaults(&world);
    world.gravity = v3(0, -9.8f, 0); world.walls = 1;
    world.bmin = v3(-1.6f, 0, -1.6f); world.bmax = v3(1.6f, 6, 1.6f);
    toss();
}

static void g_update(float dt) {
    if (mote_just_pressed(mote->input(), MOTE_BTN_A)) toss();
    mote->phys_step(&world, body, NB, dt);
    s_t += dt;
    Vec3 eye = v3(4.0f * cosf(s_t * 0.3f), 2.6f, 4.0f * sinf(s_t * 0.3f));
    Mat3 cam = mote_camera_look(eye, v3(0, 0.8f, 0));
    mote->scene_camera(&cam, eye, 80.0f);   /* world-space camera: mote_draw takes world positions */
    mote_draw(mote, s_floor, v3(0, -0.1f, 0));
    for (int i = 0; i < NB; i++) mote_draw_ex(mote, s_box, body[i].pos, body[i].orient, 1.0f);
}

static void g_overlay(uint16_t *fb) { mote->text(fb, "A  RE-TOSS", 4, 4, MOTE_RGB565(220, 228, 240)); }

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 400, .max_bodies = NB, .max_contacts = 192, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
