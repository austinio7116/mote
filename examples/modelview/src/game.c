/*
 * modelview — loads a real STL model (a fighter jet) converted by tools/stl2mesh
 * and shown through the Mote 3D pipeline. This build bakes the model at FULL detail,
 * Z-up→Y-up corrected (the STL is authored Z-up), so the STL's 6742 raw triangles
 * vertex-weld to 5741 tris split across 19 <=255-vertex chunks — a heavy 3D stress
 * test. The baker bundles those chunks into one `MoteModel fighter` (fighter.h), so
 * we draw the whole thing with a single mote_model_draw_ex() — no chunk loop, no
 * NCHUNKS, and the pool size is just `fighter_TRIS` (5741, ~240 KB draw-list + depth,
 * within the 280 KB arena).
 *
 * Controls: D-pad orbit the model · A toggle auto-spin · LB/RB zoom
 */
#include "mote_api.h"
#include "mote_build.h"
#include "fighter.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define CAM_DIST_MIN  9.0f
#define CAM_DIST_MAX 30.0f

static Mat3  model_rot;
static float cam_dist = 16.0f;
static int   auto_spin = 1;

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(22, 28, 48));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.75f, -0.5f)));
    model_rot = m3_identity();
    m3_rotate_local(&model_rot, 0, -0.4f);          /* tilt nose up a touch */
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if(mote_just_pressed(in, MOTE_BTN_A)) auto_spin = !auto_spin;

    /* LB/RB zoom in and out, clamped to a sensible orbit range */
    if(mote_pressed(in, MOTE_BTN_LB)) cam_dist += 8.0f * dt;
    if(mote_pressed(in, MOTE_BTN_RB)) cam_dist -= 8.0f * dt;
    cam_dist = mote_clampf(cam_dist, CAM_DIST_MIN, CAM_DIST_MAX);

    /* D-pad orbits the model; with no input and auto-spin on, drift slowly */
    float yaw = 0, pitch = 0;
    if(mote_pressed(in, MOTE_BTN_LEFT))  yaw   = -1.6f * dt;
    if(mote_pressed(in, MOTE_BTN_RIGHT)) yaw   =  1.6f * dt;
    if(mote_pressed(in, MOTE_BTN_UP))    pitch = -1.4f * dt;
    if(mote_pressed(in, MOTE_BTN_DOWN))  pitch =  1.4f * dt;
    if(auto_spin && yaw == 0 && pitch == 0) yaw = 0.5f * dt;

    m3_rotate_local(&model_rot, 1, yaw);
    m3_rotate_local(&model_rot, 0, pitch);
    m3_orthonormalize(&model_rot);

    /* render in world coordinates; scene_camera subtracts the camera for us */
    Vec3 cam_pos = v3(0, 0, -cam_dist);
    Mat3 cam_basis = mote_camera_look(cam_pos, v3(0, 0, 0));
    mote->scene_camera(&cam_basis, cam_pos, 50.0f);

    mote_model_draw_ex(mote, &fighter, v3(0, 0, 0), model_rot, 1.0f);
}

static void g_overlay(uint16_t *fb){
    mote_ui_panel(fb, 1, 1, 92, 11, MOTE_RGB565(16,22,40), MOTE_RGB565(80,100,150));
    mote_textf(mote, fb, 4, 3, MOTE_RGB565(200,220,255), "FIGHTER %d tris", fighter_TRIS);
    mote->text(fb, "DPAD ORBIT  A SPIN  LB/RB ZOOM", 3, 118, MOTE_RGB565(150,170,200));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = fighter_TRIS, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
