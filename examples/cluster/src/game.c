/*
 * cluster — a REAL captured 3D Gaussian-Splatting scene ("cluster fly", a
 * Postshot photogrammetry capture), converted with tools/ply2splat.py and
 * rendered by mote_splat. 12000 of the 25627 Gaussians (importance-downsampled
 * to fit the device), depth-sorted + blended across both cores.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN splat count · A spin toggle
 */
#include "mote_api.h"
#include "mote_build.h"
#include "cluster_data.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* frame the capture: orbit centre + radius chosen so the fly fills the view */
static const Vec3 k_target = { 0.0f, 0.05f, 0.0f };
#define ORBIT_R      4.0f
#define FOV          50.0f
#define SPLAT_MIN    100.0f      /* never draw fewer than this many splats */
#define ORBIT_SPEED  1.1f        /* LEFT/RIGHT orbit rate (rad/s)          */
#define SPIN_SPEED   0.25f       /* idle auto-spin rate (rad/s)            */

/* depth-sort scratch: one slot per splat, filled by scene_set_splats each frame */
static int   splat_order[CLUSTER_SPLATS_COUNT];
static float orbit_angle = 0.5f;
static float tilt_angle  = 0.18f;
static int   spinning    = 1;
static float splat_count = CLUSTER_SPLATS_COUNT;   /* live splat-count dial (UP/DOWN) */
static Vec3  cam_pos;
static Mat3  cam_basis;

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(6, 7, 12));
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_A))    spinning = !spinning;
    if (mote_pressed(in, MOTE_BTN_LEFT))  orbit_angle -= ORBIT_SPEED * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) orbit_angle += ORBIT_SPEED * dt;

    /* UP/DOWN scale how many splats are drawn — they're importance-sorted, so
     * the first N are the most significant. A live quality/fps dial. */
    if (mote_pressed(in, MOTE_BTN_UP))   splat_count += CLUSTER_SPLATS_COUNT * 0.7f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) splat_count -= CLUSTER_SPLATS_COUNT * 0.7f * dt;
    splat_count = mote_clampf(splat_count, SPLAT_MIN, (float)CLUSTER_SPLATS_COUNT);

    if (spinning) orbit_angle += SPIN_SPEED * dt;

    /* place the eye on an orbit around the capture centre */
    cam_pos = v3_add(k_target, v3( sinf(orbit_angle) * cosf(tilt_angle) * ORBIT_R,
                                   sinf(tilt_angle) * ORBIT_R,
                                  -cosf(orbit_angle) * cosf(tilt_angle) * ORBIT_R));
    cam_basis = mote_camera_look(cam_pos, k_target);

    mote->scene_begin(&cam_basis, FOV);
    mote->scene_set_splats(cluster_splats, (int)splat_count, splat_order, &cam_basis, cam_pos, FOV, 0);
}

static void g_overlay(uint16_t *fb){
    mote_ui_panel(fb, 1, 1, 74, 11, MOTE_RGB565(8, 10, 18), MOTE_RGB565(60, 80, 120));
    mote_textf(mote, fb, 4, 3, MOTE_RGB565(210, 220, 240), "SPLATS %d", (int)splat_count);

    /* live fraction bar of the splat-count dial */
    mote_ui_bar(fb, 1, 13, 74, 3, splat_count / (float)CLUSTER_SPLATS_COUNT,
                MOTE_RGB565(90, 150, 240), MOTE_RGB565(20, 26, 40));
    mote->text(fb, "LR ORBIT UD COUNT A SPIN", 2, 118, MOTE_RGB565(150, 160, 180));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_splats = CLUSTER_SPLATS_COUNT, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
