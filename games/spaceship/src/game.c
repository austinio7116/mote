/* spaceship — a 3D Mote game. Reach the engine through `mote->...`; mote_build.h gives
 * safe mesh primitives, a camera helper, and a tiny UI. */
#include "mote_api.h"
#include "mote_build.h"
#include "ship.h"        /* baked model: `static const MoteModel ship`; ship_TRIS = its tri count */

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static Mat3 s_m;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 12, 26));
    mote->scene_set_sun(v3(0.4f, 0.7f, -0.6f));
    s_m = m3_identity();
}

static void g_update(float dt) {
    m3_rotate_local(&s_m, 1, 0.9f * dt); m3_orthonormalize(&s_m);
    Mat3 cam = mote_camera_look(v3(0, 0, 0), v3(0, 0, 1));   /* eye -> target */
    mote->scene_begin(&cam, 60.0f);
    mote_model_draw_ex(mote, &ship, v3(0, 0, 4.5f), s_m, 1.0f);   /* the textured ship.mmesh (all chunks) */
}

/* Declare the pools you use so the loader sizes the arena to YOUR game.
 * The ship is textured, so its faces go to the textured-tri pool (max_tex_tris). */
static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update,
    .config = { .max_tex_tris = ship_TRIS, .max_tris = ship_TRIS, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("spaceship", "you");
