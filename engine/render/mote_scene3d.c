/*
 * Mote — scene draw-list + dual-core banded rasterisation.
 */
#include "mote_scene3d.h"
#include "mote_raster.h"
#include <string.h>

typedef struct {
    float ax, ay; uint16_t az;
    float bx, by; uint16_t bz;
    float cx, cy; uint16_t cz;
    uint16_t color;
} ScreenTri;

static ScreenTri s_tris[MOTE_SCENE_MAX_TRIS];
static int       s_ntris;
static uint16_t  s_bg = 0x0000;

void mote_scene_set_background(uint16_t rgb565) { s_bg = rgb565; }

/* Called by mote_pipe for each projected, lit, clipped screen triangle. */
void mote_emit_tri(float ax, float ay, uint16_t az,
                 float bx, float by, uint16_t bz,
                 float cx, float cy, uint16_t cz, uint16_t color) {
    if (s_ntris >= MOTE_SCENE_MAX_TRIS) return;
    ScreenTri *t = &s_tris[s_ntris++];
    t->ax = ax; t->ay = ay; t->az = az;
    t->bx = bx; t->by = by; t->bz = bz;
    t->cx = cx; t->cy = cy; t->cz = cz;
    t->color = color;
}

void mote_scene_begin(const Mat3 *cam_basis, float fov_deg) {
    s_ntris = 0;
    mote_pipe_set_camera(cam_basis, fov_deg);
}

/* Drop the draw-list without touching the camera — the OS calls this at the
 * start of every frame so a game that doesn't use the 3D scene never inherits
 * stale triangles from a previously-run game. */
void mote_scene_clear(void) { s_ntris = 0; }

int mote_scene_add_object(const MoteObject *obj) {
    return mote_pipe_draw_object(obj);
}
int mote_scene_add_object_scaled(const MoteObject *obj, float scale) {
    return mote_pipe_draw_object_scaled(obj, scale);
}

int mote_scene_tri_count(void) { return s_ntris; }

MOTE_HOT
void mote_scene_raster(uint16_t *fb, int y0, int y1) {
    /* Logical band -> physical band. */
    int py0 = y0 * MOTE_SS, py1 = y1 * MOTE_SS;
    if (py0 < 0) py0 = 0;
    if (py1 > MOTE_FB_PH) py1 = MOTE_FB_PH;

    mote_raster_set_fb(fb);
    mote_depth_clear(py0, py1);

    /* Background fill for this band. */
    for (int y = py0; y < py1; y++) {
        uint16_t *row = fb + y * MOTE_FB_PW;
        for (int x = 0; x < MOTE_FB_PW; x++) row[x] = s_bg;
    }

    const float ss = (float)MOTE_SS;
    for (int i = 0; i < s_ntris; i++) {
        const ScreenTri *t = &s_tris[i];
        mote_tri(t->ax * ss, t->ay * ss, t->az,
               t->bx * ss, t->by * ss, t->bz,
               t->cx * ss, t->cy * ss, t->cz,
               t->color, py0, py1);
    }
}
