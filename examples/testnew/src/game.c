/* testnew — a minimal Mote game module: one lit cube, slowly rotating.
 *
 * Reach the engine through `mote->...`; mote_build.h gives safe mesh
 * primitives, a camera helper, and a tiny UI. */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static const Mesh *cube_mesh;
static Mat3 cube_rot;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 12, 26));
    mote->scene_set_sun(v3(0.4f, 0.7f, -0.6f));

    cube_mesh = mote_mesh_box(mote, 0.3f, 0.3f, 0.3f, MOTE_RGB565(214, 180, 230));
    cube_rot = m3_identity();
}

static void g_update(float dt) {
    /* spin the cube slowly about its local Y axis */
    m3_rotate_local(&cube_rot, 1, 0.9f * dt);
    m3_orthonormalize(&cube_rot);

    /* camera sits at the origin looking down +Z */
    Mat3 cam_basis = mote_camera_look(v3(0, 0, 0), v3(0, 0, 1));   /* eye -> target */
    mote->scene_begin(&cam_basis, 60.0f);

    mote_draw_ex(mote, cube_mesh, v3(0, 0, 4.5f), cube_rot, 1.0f);
}

/* Declare the pools you use so the loader sizes the arena to YOUR game. */
static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update,
    .config = { .max_tris = 256, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
