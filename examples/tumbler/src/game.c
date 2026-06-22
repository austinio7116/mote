/*
 * tumbler — a second Mote game module, to give the launcher real choices.
 *
 * A vivid box tumbling on all three axes, with a small sphere orbiting it for
 * a bit of life, over a deep teal field. Mesh + camera are built with the
 * sdk/mote_build.h helpers (no hand-rolled int8 geometry). Same engine, same
 * ABI as hello-mesh; only the content differs.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static const Mesh *box_mesh;
static const Mesh *orb_mesh;
static Mat3  box_rot;
static Vec3  cam_pos;
static Mat3  cam_basis;
static float elapsed;          /* seconds since start, drives the orbit */

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 30, 34));
    mote->scene_set_sun(v3_norm(v3(-0.3f, 0.7f, -0.6f)));

    /* a chunky box (neon magenta) + a small teal sphere that orbits it */
    box_mesh = mote_mesh_box(mote, 0.95f, 0.95f, 0.95f, MOTE_RGB565(245, 70, 165));
    orb_mesh = mote_mesh_sphere(mote, 0.32f, 12, MOTE_RGB565(80, 240, 210));

    box_rot = m3_identity();
    elapsed = 0.0f;

    /* look slightly down at the origin from a pulled-back eye */
    cam_pos   = v3(0.0f, 1.1f, -4.6f);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0, 0));
}

static void g_update(float dt) {
    (void)mote->input();
    elapsed += dt;

    /* tumble on all three local axes */
    m3_rotate_local(&box_rot, 0, 0.7f * dt);
    m3_rotate_local(&box_rot, 1, 1.1f * dt);
    m3_rotate_local(&box_rot, 2, 0.5f * dt);
    m3_orthonormalize(&box_rot);

    /* render in world coordinates; scene_camera subtracts the camera for us */
    mote->scene_camera(&cam_basis, cam_pos, 55.0f);

    mote_draw_ex(mote, box_mesh, v3(0, 0, 0), box_rot, 1.0f);

    /* small sphere orbiting in the XZ plane, bobbing slightly in Y */
    float angle = elapsed * 1.6f;
    Vec3 orb_pos = v3(cosf(angle) * 1.9f, sinf(elapsed * 2.1f) * 0.4f, sinf(angle) * 1.9f);
    mote_draw(mote, orb_mesh, orb_pos);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .render_band = 0, .overlay = 0,
    .config = { .max_tris = 200, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
