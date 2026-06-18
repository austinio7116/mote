/*
 * Mote — scene draw-list + dual-core banded rasterisation.
 */
#include "mote_scene3d.h"
#include "mote_raster.h"
#include "mote_config.h"
#include <math.h>
#include <string.h>

typedef struct {
    float ax, ay; uint16_t az;
    float bx, by; uint16_t bz;
    float cx, cy; uint16_t cz;
    uint16_t color;
} ScreenTri;

/* Sphere impostor: a screen disc shaded per-pixel as a sphere (front
 * hemisphere normal reconstructed from the disc), depth-tested + writing. */
typedef struct {
    float sx, sy, sr;   /* logical screen centre + radius */
    float vz, radius;   /* view-space centre depth + world radius */
    uint16_t color;
} ScreenSphere;

static ScreenTri    s_tris[MOTE_SCENE_MAX_TRIS];
static int          s_ntris;
static ScreenSphere s_spheres[MOTE_SCENE_MAX_SPHERES];
static int          s_nspheres;
static uint16_t     s_bg = 0x0000;

static inline uint16_t shade565(uint16_t c, float sh) {
    int r = (int)(((c >> 11) & 0x1F) * sh);
    int g = (int)(((c >> 5) & 0x3F) * sh);
    int b = (int)((c & 0x1F) * sh);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

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
    s_nspheres = 0;
    mote_pipe_set_camera(cam_basis, fov_deg);
}

/* Drop the draw-list without touching the camera — the OS calls this at the
 * start of every frame so a game that doesn't use the 3D scene never inherits
 * stale triangles from a previously-run game. */
void mote_scene_clear(void) { s_ntris = 0; s_nspheres = 0; }

/* Add a sphere (camera-relative world position). Projected now; shaded as a
 * per-pixel impostor during the band raster — perfect spheres, cheap. */
int mote_scene_add_sphere(Vec3 cam_rel_pos, float radius, uint16_t color) {
    if (s_nspheres >= MOTE_SCENE_MAX_SPHERES) return 0;
    const Mat3 *cam = mote_pipe_camera();
    Vec3 v = m3_mul_v3_t(cam, cam_rel_pos);          /* world -> view */
    if (v.z <= MOTE_NEAR) return 0;
    float focal = mote_pipe_focal(), inv = 1.0f / v.z;
    ScreenSphere *s = &s_spheres[s_nspheres++];
    s->sx = (MOTE_FB_W * 0.5f) + focal * v.x * inv;
    s->sy = (MOTE_FB_H * 0.5f) - focal * v.y * inv;
    s->sr = focal * radius * inv;
    s->vz = v.z; s->radius = radius; s->color = color;
    return 1;
}

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

    /* Sphere impostors (after tris; depth-tested + writing). */
    if (s_nspheres > 0) {
        Vec3 sun = mote_pipe_sun_view();
        uint16_t *depth = mote_depth_buffer();
        for (int i = 0; i < s_nspheres; i++) {
            const ScreenSphere *s = &s_spheres[i];
            float cx = s->sx * ss, cy = s->sy * ss, r = s->sr * ss;
            if (r < 0.5f) continue;
            int minx = (int)(cx - r); if (minx < 0) minx = 0;
            int maxx = (int)(cx + r) + 1; if (maxx > MOTE_FB_PW) maxx = MOTE_FB_PW;
            int miny = (int)(cy - r); if (miny < py0) miny = py0;
            int maxy = (int)(cy + r) + 1; if (maxy > py1) maxy = py1;
            float invr = 1.0f / r;
            for (int y = miny; y < maxy; y++) {
                float ndy = (y + 0.5f - cy) * invr;
                uint16_t *frow = fb + y * MOTE_FB_PW;
                uint16_t *drow = depth + y * MOTE_FB_PW;
                for (int x = minx; x < maxx; x++) {
                    float ndx = (x + 0.5f - cx) * invr;
                    float rr = ndx * ndx + ndy * ndy;
                    if (rr > 1.0f) continue;
                    float nz = sqrtf(1.0f - rr);
                    /* view-space front-hemisphere normal = (ndx, -ndy, -nz) */
                    float ndotl = ndx * sun.x - ndy * sun.y - nz * sun.z;
                    float sh = 0.22f + (ndotl > 0.0f ? 0.78f * ndotl : 0.0f);
                    float zf = s->vz - s->radius * nz;
                    if (zf < MOTE_NEAR) zf = MOTE_NEAR;
                    uint16_t d = (uint16_t)(MOTE_DEPTH_K / zf);
                    int idx = x;
                    if (d > drow[idx]) { drow[idx] = d; frow[idx] = shade565(s->color, sh); }
                }
            }
        }
    }
}
