/*
 * tumbler — a second Mote game module, to give the launcher real choices.
 *
 * A neon cube tumbling on all three axes over a dark-green field. Same engine,
 * same ABI as hello-mesh; only the content differs. Built as its own .so /
 * .mote module and loaded independently.
 */
#include "mote_api.h"
#include "cube2.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static Mat3 s_m;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(6, 26, 14));
    mote->scene_set_sun(v3(-0.3f, 0.6f, -0.7f));
    s_m = m3_identity();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();

    m3_rotate_local(&s_m, 0, 0.7f * dt);
    m3_rotate_local(&s_m, 1, 1.1f * dt);
    m3_rotate_local(&s_m, 2, 0.5f * dt);
    m3_orthonormalize(&s_m);

    Mat3 cam = m3_identity();
    mote->scene_begin(&cam, 55.0f);
    MoteObject obj = { .pos = v3(0, 0, 4.2f), .basis = s_m, .mesh = &k_mesh };
    mote->scene_add_object(&obj);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .render_band = 0, .overlay = 0,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
