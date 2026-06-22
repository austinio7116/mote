/*
 * zelda — a REAL captured 3D Gaussian-Splatting scene (a sculpted Zelda bust),
 * converted with tools/ply2splat.py and rendered by mote_splat. 12000 of 779690
 * Gaussians (importance-downsampled to fit the device), depth-sorted + blended
 * across both cores.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN splat count · A spin toggle
 */
#include "mote_api.h"
#include "mote_build.h"
#include "zelda_data.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* frame the bust: lift the orbit centre so the head sits mid-screen */
static const Vec3 k_target = { 0.0f, 0.10f, 0.0f };
#define ORBIT_R    4.0f
#define FOV        50.0f
#define MIN_SPLATS 100.0f

static int   splat_order[ZELDA_SPLATS_COUNT];      /* depth-sort scratch */
static float orbit_angle = 0.5f;                   /* yaw around the bust (LEFT/RIGHT, A spin) */
static float orbit_tilt  = 0.12f;                  /* fixed pitch */
static int   spinning    = 1;                      /* auto-orbit toggle (A) */
static float splat_count = ZELDA_SPLATS_COUNT;     /* live splat-count dial (UP/DOWN) */

static Vec3  cam_pos;
static Mat3  cam_basis;

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(6, 7, 12));
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_A))    spinning = !spinning;
    if (mote_pressed(in, MOTE_BTN_LEFT))      orbit_angle -= 1.1f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT))     orbit_angle += 1.1f * dt;

    /* UP/DOWN scale how many splats are drawn — they're importance-sorted, so
     * the first N are the most significant. A live quality/fps dial. */
    if (mote_pressed(in, MOTE_BTN_UP))        splat_count += ZELDA_SPLATS_COUNT * 0.7f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))      splat_count -= ZELDA_SPLATS_COUNT * 0.7f * dt;
    splat_count = mote_clampf(splat_count, MIN_SPLATS, (float)ZELDA_SPLATS_COUNT);

    if (spinning) orbit_angle += 0.25f * dt;

    /* place the camera on an orbit around k_target, then look back at it */
    cam_pos = v3_add(k_target, v3( sinf(orbit_angle) * cosf(orbit_tilt) * ORBIT_R,
                                   sinf(orbit_tilt) * ORBIT_R,
                                  -cosf(orbit_angle) * cosf(orbit_tilt) * ORBIT_R));
    cam_basis = mote_camera_look(cam_pos, k_target);

    mote->scene_begin(&cam_basis, FOV);
    mote->scene_set_splats(zelda_splats, (int)splat_count, splat_order, &cam_basis, cam_pos, FOV, 0);
}

static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 1, 1, 74, 11, MOTE_RGB565(8, 10, 18), MOTE_RGB565(60, 80, 120));
    mote_textf(mote, fb, 4, 3, MOTE_RGB565(210, 220, 240), "SPLATS %d", (int)splat_count);

    mote_ui_bar(fb, 1, 13, 74, 3, splat_count / (float)ZELDA_SPLATS_COUNT,
                MOTE_RGB565(90, 150, 240), MOTE_RGB565(20, 26, 40));
    mote->text(fb, "LR ORBIT UD COUNT A SPIN", 2, 118, MOTE_RGB565(150, 160, 180));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_splats = ZELDA_SPLATS_COUNT, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
