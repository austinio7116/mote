/*
 * cluster — a REAL Gaussian-splat scene capture (a Zelda render, 12000 of 779690 splats).
 * Postshot photogrammetry capture), converted with tools/ply2splat.py and
 * rendered by mote_splat. 6000 of the 25627 Gaussians (importance-downsampled
 * to fit the device), depth-sorted + blended across both cores.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN tilt · A spin toggle · MENU exit
 */
#include "mote_api.h"
#include "zelda_data.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static int s_order[ZELDA_SPLATS_COUNT];
static float s_orbit = 0.5f, s_tilt = 0.2f; static int s_spin = 1;
static float s_countf = ZELDA_SPLATS_COUNT;   /* live splat-count dial (UP/DOWN) */
static Vec3 s_cam; static Mat3 s_basis;

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

    float r = 4.0f;
    s_cam = v3(sinf(s_orbit)*cosf(s_tilt)*r, sinf(s_tilt)*r, -cosf(s_orbit)*cosf(s_tilt)*r);
    Vec3 fwd = v3_norm(v3_sub(v3(0,0,0), s_cam));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    s_basis.r[0] = right; s_basis.r[1] = v3_cross(fwd, right); s_basis.r[2] = fwd;

    mote->scene_begin(&s_basis, 50.0f);
    mote->scene_set_splats(zelda_splats, (int)s_countf, s_order, &s_basis, s_cam, 50.0f, 0);
}

static void g_overlay(uint16_t *fb){
    char b[24]; int n = (int)s_countf;
    /* tiny int->str (no snprintf dependency in the module) */
    char num[8]; int p = 0; if (n == 0) num[p++]='0';
    char tmp[8]; int t2 = 0; while (n > 0) { tmp[t2++] = '0' + n%10; n/=10; }
    while (t2 > 0) num[p++] = tmp[--t2]; num[p] = 0;
    int q = 0; const char *pre = "SPLATS "; while (*pre) b[q++]=*pre++;
    for (int i = 0; num[i]; i++) b[q++] = num[i]; b[q] = 0;
    mote->text(fb, b, 3, 3, MOTE_RGB565(210,220,240));
    mote->text(fb, "LR ORBIT UD COUNT A SPIN", 2, 118, MOTE_RGB565(150,160,180));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
