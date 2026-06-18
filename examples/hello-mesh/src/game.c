/*
 * hello-mesh — as a loadable Mote game MODULE.
 *
 * Identical behaviour to the Phase 0 standalone main.c, but here the game
 * owns no loop and links no engine code: it is handed the engine jump table
 * (`mote`) and the OS drives its vtable callbacks. This is the form the OS
 * loads dynamically (host: dlopen .so; device: map/copy a .mote).
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "mote_api.h"
#include "cube.h"

MOTE_GAME_MODULE();   /* exports mote_game_abi_version + mote_game_register, sets `mote` */

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();    /* device .mote flash header (linker symbols from game.ld) */
#endif

static Mat3  s_cube;
static float s_spin_x, s_spin_y;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 12, 24));
    mote->scene_set_sun(v3(0.4f, 0.7f, -0.6f));
    s_cube = m3_identity();
    m3_rotate_local(&s_cube, 0, 0.5f);   /* start tilted so 3 faces show */
    m3_rotate_local(&s_cube, 1, 0.7f);
    s_spin_x = 0.6f;
    s_spin_y = 0.9f;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_pressed(in, MOTE_BTN_UP))    s_spin_x -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_spin_x += 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_spin_y -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_spin_y += 1.5f * dt;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        s_cube = m3_identity(); s_spin_x = 0.6f; s_spin_y = 0.9f;
    }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();

    m3_rotate_local(&s_cube, 0, s_spin_x * dt);
    m3_rotate_local(&s_cube, 1, s_spin_y * dt);
    m3_orthonormalize(&s_cube);

    Mat3 cam = m3_identity();
    mote->scene_begin(&cam, 60.0f);
    MoteObject obj = { .pos = v3(0, 0, 4.5f), .basis = s_cube, .mesh = &k_cube_mesh };
    mote->scene_add_object(&obj);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,    /* use the built-in 3D scene rasteriser */
    .overlay = 0,
};

static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
