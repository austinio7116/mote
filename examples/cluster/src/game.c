/*
 * cluster — a REAL captured 3D Gaussian-Splatting scene ("cluster fly", a
 * Postshot photogrammetry capture), converted with tools/ply2splat.py and
 * rendered by mote_splat. 6000 of the 25627 Gaussians (importance-downsampled
 * to fit the device), depth-sorted + blended across both cores.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN tilt · A spin toggle · MENU exit
 */
#include "mote_api.h"
#include "cluster_data.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static int s_order[CLUSTER_SPLATS_COUNT];
static float s_orbit = 0.5f, s_tilt = 0.2f; static int s_spin = 1;
static Vec3 s_cam; static Mat3 s_basis;

static void g_init(void){ mote->scene_set_background(MOTE_RGB565(6, 7, 12)); }

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    s_spin = !s_spin;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_orbit -= 1.1f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_orbit += 1.1f*dt;
    if (mote_pressed(in, MOTE_BTN_UP))    s_tilt += 0.9f*dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_tilt -= 0.9f*dt;
    if (s_spin) s_orbit += 0.25f*dt;
    if (s_tilt >  1.3f) s_tilt =  1.3f;
    if (s_tilt < -1.3f) s_tilt = -1.3f;

    float r = 4.0f;
    s_cam = v3(sinf(s_orbit)*cosf(s_tilt)*r, sinf(s_tilt)*r, -cosf(s_orbit)*cosf(s_tilt)*r);
    Vec3 fwd = v3_norm(v3_sub(v3(0,0,0), s_cam));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    s_basis.r[0] = right; s_basis.r[1] = v3_cross(fwd, right); s_basis.r[2] = fwd;

    mote->scene_begin(&s_basis, 50.0f);
    mote->scene_set_splats(cluster_splats, CLUSTER_SPLATS_COUNT, s_order, &s_basis, s_cam, 50.0f, 0);
}

static void g_overlay(uint16_t *fb){
    mote->text(fb, "GAUSSIAN CAPTURE", 3, 3, MOTE_RGB565(210,220,240));
    mote->text(fb, "LR/UD ORBIT  A SPIN", 3, 118, MOTE_RGB565(150,160,180));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
