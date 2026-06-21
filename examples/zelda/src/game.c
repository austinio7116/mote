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

static int s_order[ZELDA_SPLATS_COUNT];
static float s_orbit = 0.5f, s_tilt = 0.12f; static int s_spin = 1;
static float s_countf = ZELDA_SPLATS_COUNT;   /* live splat-count dial (UP/DOWN) */
static Vec3 s_cam; static Mat3 s_basis;

/* frame the bust: lift the orbit centre so the head sits mid-screen */
static const Vec3 k_target = { 0.0f, 0.10f, 0.0f };
#define ORBIT_R 4.0f

static void g_init(void){ mote->scene_set_background(MOTE_RGB565(6, 7, 12)); }

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A))    s_spin = !s_spin;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_orbit -= 1.1f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_orbit += 1.1f*dt;
    /* UP/DOWN scale how many splats are drawn — they're importance-sorted, so
     * the first N are the most significant. A live quality/fps dial. */
    if (mote_pressed(in, MOTE_BTN_UP))   s_countf += ZELDA_SPLATS_COUNT * 0.7f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) s_countf -= ZELDA_SPLATS_COUNT * 0.7f * dt;
    if (s_countf > ZELDA_SPLATS_COUNT) s_countf = ZELDA_SPLATS_COUNT;
    if (s_countf < 100.0f) s_countf = 100.0f;
    if (s_spin) s_orbit += 0.25f*dt;

    s_cam = v3_add(k_target, v3(sinf(s_orbit)*cosf(s_tilt)*ORBIT_R,
                                sinf(s_tilt)*ORBIT_R,
                               -cosf(s_orbit)*cosf(s_tilt)*ORBIT_R));
    s_basis = mote_camera_look(s_cam, k_target);

    mote->scene_begin(&s_basis, 50.0f);
    mote->scene_set_splats(zelda_splats, (int)s_countf, s_order, &s_basis, s_cam, 50.0f, 0);
}

static void g_overlay(uint16_t *fb){
    char b[16]; int q = 0; const char *pre = "SPLATS ";
    while (*pre) b[q++] = *pre++;
    q += mote_itoa((int)s_countf, b + q);
    mote_ui_panel(fb, 1, 1, 74, 11, MOTE_RGB565(8, 10, 18), MOTE_RGB565(60, 80, 120));
    mote->text(fb, b, 4, 3, MOTE_RGB565(210, 220, 240));
    mote_ui_bar(fb, 1, 13, 74, 3, s_countf / (float)ZELDA_SPLATS_COUNT,
                MOTE_RGB565(90, 150, 240), MOTE_RGB565(20, 26, 40));
    mote->text(fb, "LR ORBIT UD COUNT A SPIN", 2, 118, MOTE_RGB565(150, 160, 180));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_splats = ZELDA_SPLATS_COUNT, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
