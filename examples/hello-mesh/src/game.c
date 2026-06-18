/*
 * hello-mesh — as a loadable ThumbyEngine game MODULE.
 *
 * Identical behaviour to the Phase 0 standalone main.c, but here the game
 * owns no loop and links no engine code: it is handed the engine jump table
 * (`te`) and the OS drives its vtable callbacks. This is the form the OS
 * loads dynamically (host: dlopen .so; device: map/copy a .tgm).
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "te_api.h"
#include "cube.h"

TE_GAME_MODULE();   /* exports te_game_abi_version + te_game_register, sets `te` */

static Mat3  s_cube;
static float s_spin_x, s_spin_y;

static void g_init(void) {
    te->scene_set_background(TE_RGB565(12, 12, 24));
    te->scene_set_sun(v3(0.4f, 0.7f, -0.6f));
    s_cube = m3_identity();
    m3_rotate_local(&s_cube, 0, 0.5f);   /* start tilted so 3 faces show */
    m3_rotate_local(&s_cube, 1, 0.7f);
    s_spin_x = 0.6f;
    s_spin_y = 0.9f;
}

static void g_update(float dt) {
    const TeInput *in = te->input();
    if (te_pressed(in, TE_BTN_UP))    s_spin_x -= 1.5f * dt;
    if (te_pressed(in, TE_BTN_DOWN))  s_spin_x += 1.5f * dt;
    if (te_pressed(in, TE_BTN_LEFT))  s_spin_y -= 1.5f * dt;
    if (te_pressed(in, TE_BTN_RIGHT)) s_spin_y += 1.5f * dt;
    if (te_just_pressed(in, TE_BTN_A)) {
        s_cube = m3_identity(); s_spin_x = 0.6f; s_spin_y = 0.9f;
    }
    if (te_just_pressed(in, TE_BTN_MENU)) te->exit_to_launcher();

    m3_rotate_local(&s_cube, 0, s_spin_x * dt);
    m3_rotate_local(&s_cube, 1, s_spin_y * dt);
    m3_orthonormalize(&s_cube);

    Mat3 cam = m3_identity();
    te->scene_begin(&cam, 60.0f);
    TeObject obj = { .pos = v3(0, 0, 4.5f), .basis = s_cube, .mesh = &k_cube_mesh };
    te->scene_add_object(&obj);
}

static const TeGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,    /* use the built-in 3D scene rasteriser */
    .overlay = 0,
};

static const TeGameVtbl *te_game_vtbl(void) { return &k_vtbl; }
