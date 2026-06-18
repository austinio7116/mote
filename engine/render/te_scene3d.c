/*
 * ThumbyEngine — scene draw-list + dual-core banded rasterisation.
 */
#include "te_scene3d.h"
#include "te_raster.h"
#include <string.h>

typedef struct {
    float ax, ay; uint16_t az;
    float bx, by; uint16_t bz;
    float cx, cy; uint16_t cz;
    uint16_t color;
} ScreenTri;

static ScreenTri s_tris[TE_SCENE_MAX_TRIS];
static int       s_ntris;
static uint16_t  s_bg = 0x0000;

void te_scene_set_background(uint16_t rgb565) { s_bg = rgb565; }

/* Called by te_pipe for each projected, lit, clipped screen triangle. */
void te_emit_tri(float ax, float ay, uint16_t az,
                 float bx, float by, uint16_t bz,
                 float cx, float cy, uint16_t cz, uint16_t color) {
    if (s_ntris >= TE_SCENE_MAX_TRIS) return;
    ScreenTri *t = &s_tris[s_ntris++];
    t->ax = ax; t->ay = ay; t->az = az;
    t->bx = bx; t->by = by; t->bz = bz;
    t->cx = cx; t->cy = cy; t->cz = cz;
    t->color = color;
}

void te_scene_begin(const Mat3 *cam_basis, float fov_deg) {
    s_ntris = 0;
    te_pipe_set_camera(cam_basis, fov_deg);
}

int te_scene_add_object(const TeObject *obj) {
    return te_pipe_draw_object(obj);
}
int te_scene_add_object_scaled(const TeObject *obj, float scale) {
    return te_pipe_draw_object_scaled(obj, scale);
}

int te_scene_tri_count(void) { return s_ntris; }

TE_HOT
void te_scene_raster(uint16_t *fb, int y0, int y1) {
    /* Logical band -> physical band. */
    int py0 = y0 * TE_SS, py1 = y1 * TE_SS;
    if (py0 < 0) py0 = 0;
    if (py1 > TE_FB_PH) py1 = TE_FB_PH;

    te_raster_set_fb(fb);
    te_depth_clear(py0, py1);

    /* Background fill for this band. */
    for (int y = py0; y < py1; y++) {
        uint16_t *row = fb + y * TE_FB_PW;
        for (int x = 0; x < TE_FB_PW; x++) row[x] = s_bg;
    }

    const float ss = (float)TE_SS;
    for (int i = 0; i < s_ntris; i++) {
        const ScreenTri *t = &s_tris[i];
        te_tri(t->ax * ss, t->ay * ss, t->az,
               t->bx * ss, t->by * ss, t->bz,
               t->cx * ss, t->cy * ss, t->cz,
               t->color, py0, py1);
    }
}
