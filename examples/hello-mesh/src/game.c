/*
 * hello-mesh — the first loadable Mote game MODULE a new developer reads.
 *
 * A clean spinning-shape "hello": it draws a model BAKED in the Mote Studio model
 * editor — src/scene.h, a `static const MoteModel scene` — lit by the engine's sun.
 * Edit the model in the Studio's Mesh/Rig tabs, hit Bake .h, rebuild, and this
 * updates. The game owns no loop and links no engine code — it is handed the engine
 * jump table (`mote`) and the OS drives its vtable callbacks. This is the form the OS
 * loads dynamically (host: dlopen .so; device: map/copy a .mote).
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "scene.h"        /* baked model: `static const MoteModel scene`; scene_TRIS = its tri count */

MOTE_GAME_MODULE();   /* exports mote_game_abi_version + mote_game_register, sets `mote` */

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();    /* device .mote flash header (linker symbols from game.ld) */
#endif

static Mat3  model_rot;
static Vec3  cam_pos;
static Mat3  cam_basis;
static float spin_x, spin_y;        /* current spin rate about X and Y */

static void reset_spin(void) {
    model_rot = m3_identity();
    m3_rotate_local(&model_rot, 0, 0.5f);   /* start tilted so the form reads in 3D */
    m3_rotate_local(&model_rot, 1, 0.7f);
    spin_x = 0.6f;
    spin_y = 0.9f;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(12, 14, 28));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.7f, -0.6f)));

    cam_pos   = v3(0.0f, 0.4f, -6.2f);      /* pulled back to frame the whole baked model */
    cam_basis = mote_camera_look(cam_pos, v3(0, 0, 0));
    reset_spin();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    /* D-pad nudges the spin rate on each axis */
    if (mote_pressed(in, MOTE_BTN_UP))    spin_x -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  spin_x += 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_LEFT))  spin_y -= 1.5f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) spin_y += 1.5f * dt;
    if (mote_just_pressed(in, MOTE_BTN_A)) reset_spin();

    m3_rotate_local(&model_rot, 0, spin_x * dt);
    m3_rotate_local(&model_rot, 1, spin_y * dt);
    m3_orthonormalize(&model_rot);

    /* render in world coordinates; scene_camera subtracts the camera for us */
    mote->scene_camera(&cam_basis, cam_pos, 60.0f);
    mote_model_draw_ex(mote, &scene, v3(0, 0, 0), model_rot, 1.0f);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,    /* use the built-in 3D scene rasteriser */
    .overlay = 0,
    .config = { .max_tris = scene_TRIS, .max_tex_tris = scene_TRIS, .depth = 1 },   /* tex-tri pool so a textured scene renders (not flat) */
};

static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
