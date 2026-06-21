/*
 * hello-mesh — the first loadable Mote game MODULE a new developer reads.
 *
 * A clean spinning-shape "hello": a faceted cylinder built with the
 * sdk/mote_build.h helpers (mesh + camera), lit by the engine's sun. The game
 * owns no loop and links no engine code — it is handed the engine jump table
 * (`mote`) and the OS drives its vtable callbacks. This is the form the OS
 * loads dynamically (host: dlopen .so; device: map/copy a .mote).
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();   /* exports mote_game_abi_version + mote_game_register, sets `mote` */

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();    /* device .mote flash header (linker symbols from game.ld) */
#endif

static const Mesh *s_mesh;
static Mat3  s_rot;
static Vec3  cam_pos;
static Mat3  cam_basis;
static float s_spin_x, s_spin_y;

static void reset_spin(void) {
    s_rot = m3_identity();
    m3_rotate_local(&s_rot, 0, 0.5f);   /* start tilted so the caps + sides show */
    m3_rotate_local(&s_rot, 1, 0.7f);
    s_spin_x = 0.6f;
    s_spin_y = 0.9f;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 14, 28));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.7f, -0.6f)));

    /* a short faceted cylinder — rounder than a cube, still obviously spinning */
    s_mesh = mote_mesh_cylinder(mote, 0.85f, 0.95f, 14, MOTE_RGB565(90, 190, 245));

    cam_pos   = v3(0.0f, 0.6f, -4.4f);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0, 0));
    reset_spin();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_pressed(in, MOTE_BTN_UP))    s_spin_x -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_spin_x += 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_spin_y -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_spin_y += 1.5f * dt;
    if (mote_just_pressed(in, MOTE_BTN_A)) reset_spin();

    m3_rotate_local(&s_rot, 0, s_spin_x * dt);
    m3_rotate_local(&s_rot, 1, s_spin_y * dt);
    m3_orthonormalize(&s_rot);

    mote->scene_begin(&cam_basis, 60.0f);
    MoteObject obj = { .pos = v3_sub(v3(0, 0, 0), cam_pos), .basis = s_rot, .mesh = s_mesh };
    mote->scene_add_object(&obj);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,    /* use the built-in 3D scene rasteriser */
    .overlay = 0,
    .config = { .max_tris = 200, .depth = 1 },
};

static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
