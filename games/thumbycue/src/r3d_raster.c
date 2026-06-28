/*
 * ThumbyElite — flat-shaded depth-tested triangle rasterizer.
 *
 * Technique: half-space (edge function) rasterisation with incremental
 * row/column stepping (Fabian Giesen's optimised basic rasteriser), as
 * proven in the Tiny Game Engine's engine_draw_filled_triangle_depth —
 * but flat-colour only, with depth interpolated by plane equation instead
 * of per-pixel barycentrics, so the inner loop is 4 fadds + 2 compares.
 */
#include "r3d_raster.h"
#include <math.h>
#include <string.h>

/* All coordinates and row bands in this file are PHYSICAL pixels
 * (R3D_FB_W x R3D_FB_H). r3d_scene_raster scales up from logical space
 * before calling in; with R3D_SS == 1 the two spaces are identical. */

static uint16_t *s_fb;
/* Depth buffer (32 KB). On Mote it's arena-allocated and handed in via r3d_set_depth()
 * so it doesn't sit in the module's 128 KB static RAM; both cores read/write disjoint
 * row bands of it during the screen-half split. */
static uint16_t *s_depth;

void r3d_raster_set_fb(uint16_t *fb) {
    s_fb = fb;
}

size_t r3d_depth_bytes(void) { return (size_t)R3D_FB_W * R3D_FB_H * sizeof(uint16_t); }
void   r3d_set_depth(uint16_t *d) { s_depth = d; }

uint16_t *r3d_depth_buffer(void) { return s_depth; }

void r3d_depth_clear(int y_min, int y_max) {
    if (y_min < 0) y_min = 0;
    if (y_max > R3D_FB_H) y_max = R3D_FB_H;
    if (y_max > y_min)
        memset(s_depth + y_min * R3D_FB_W, 0,
               (size_t)(y_max - y_min) * R3D_FB_W * sizeof(uint16_t));
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void r3d_point(int x, int y, uint16_t d, uint16_t color, int size,
               int y_min, int y_max) {
    for (int dy = 0; dy < size; dy++) {
        int py = y + dy;
        if (py < y_min || py >= y_max) continue;
        uint16_t *fb_row = s_fb + py * R3D_FB_W;
        uint16_t *dp_row = s_depth + py * R3D_FB_W;
        for (int dx = 0; dx < size; dx++) {
            int px = x + dx;
            if ((unsigned)px >= R3D_FB_W) continue;
            if (d > dp_row[px]) fb_row[px] = color;
        }
    }
}

void r3d_disc(int cx, int cy, uint16_t d, int r, uint16_t color,
              int y_min, int y_max) {
    if (r < 1) r = 1;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < y_min || py >= y_max) continue;
        int half = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - half, x1 = cx + half;
        if (x0 < 0) x0 = 0;
        if (x1 > R3D_FB_W - 1) x1 = R3D_FB_W - 1;
        uint16_t *fb_row = s_fb + py * R3D_FB_W;
        uint16_t *dp_row = s_depth + py * R3D_FB_W;
        for (int px = x0; px <= x1; px++)
            if (d > dp_row[px]) fb_row[px] = color;
    }
}

void r3d_line(float x0, float y0, uint16_t d0,
              float x1, float y1, uint16_t d1,
              uint16_t color, int y_min, int y_max) {
    float dx = x1 - x0, dy = y1 - y0;
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (int)(adx > ady ? adx : ady) + 1;
    if (steps > 256 * R3D_SS) steps = 256 * R3D_SS;
    float inv = 1.0f / (float)steps;
    float sx = dx * inv, sy = dy * inv, sd = ((float)d1 - (float)d0) * inv;
    float px = x0, py = y0, pd = (float)d0;
    for (int i = 0; i <= steps; i++) {
        int ix = (int)px, iy = (int)py;
#if R3D_SS == 1
        if (iy >= y_min && iy < y_max && (unsigned)ix < R3D_FB_W) {
            int idx = iy * R3D_FB_W + ix;
            if ((uint16_t)pd > s_depth[idx]) s_fb[idx] = color;
        }
#else
        /* Stamp an R3D_SS block: beams/trails keep their device apparent
         * thickness, just with a smoother (subpixel) path. */
        for (int by = 0; by < R3D_SS; by++) {
            int yy = iy + by;
            if (yy < y_min || yy >= y_max) continue;
            for (int bx = 0; bx < R3D_SS; bx++) {
                int xx = ix + bx;
                if ((unsigned)xx >= R3D_FB_W) continue;
                int idx = yy * R3D_FB_W + xx;
                if ((uint16_t)pd > s_depth[idx]) s_fb[idx] = color;
            }
        }
#endif
        px += sx; py += sy; pd += sd;
    }
}

/* `write` is passed by value (NOT a shared global) so the two cores can raster
 * concurrently — one drawing depth-writing geometry while the other draws the
 * no-write lips — without racing on a global depth-write flag. */
static void tri_raster(float ax, float ay, uint16_t az,
             float bx, float by, uint16_t bz,
             float cx, float cy, uint16_t cz,
             uint16_t color, int y_min, int y_max, int write) {
    /* Signed area*2. Screen-clockwise => positive. <=0 is backfacing or
     * degenerate — cull. (Winding is consistent because the pipe preserves
     * mesh winding and mirrors it on projection.) */
    float area2 = edge(ax, ay, bx, by, cx, cy);
    if (area2 <= 0.0f) return;

    /* Bounding box, clamped to screen and the caller's row band. */
    float fminx = ax < bx ? ax : bx; if (cx < fminx) fminx = cx;
    float fminy = ay < by ? ay : by; if (cy < fminy) fminy = cy;
    float fmaxx = ax > bx ? ax : bx; if (cx > fmaxx) fmaxx = cx;
    float fmaxy = ay > by ? ay : by; if (cy > fmaxy) fmaxy = cy;

    int min_x = (int)fminx; if (min_x < 0) min_x = 0;
    int min_y = (int)fminy; if (min_y < y_min) min_y = y_min;
    int max_x = (int)fmaxx; if (max_x > R3D_FB_W - 1) max_x = R3D_FB_W - 1;
    int max_y = (int)fmaxy; if (max_y >= y_max) max_y = y_max - 1;
    if (min_x > max_x || min_y > max_y) return;

    /* Edge functions at the (pixel-centre) top-left corner of the bbox. */
    float px0 = (float)min_x + 0.5f, py0 = (float)min_y + 0.5f;
    float w0_row = edge(bx, by, cx, cy, px0, py0);   /* weight of A */
    float w1_row = edge(cx, cy, ax, ay, px0, py0);   /* weight of B */
    float w2_row = edge(ax, ay, bx, by, px0, py0);   /* weight of C */

    /* Per-pixel / per-row steps: d(edge(a,b,p))/dpx = ay-by,
     * d/dpy = bx-ax (applied to each edge's own endpoints). */
    float w0_dx = by - cy, w0_dy = cx - bx;
    float w1_dx = cy - ay, w1_dy = ax - cx;
    float w2_dx = ay - by, w2_dy = bx - ax;

    /* Depth plane: z(p) = (w0*az + w1*bz + w2*cz) / area2, which is linear
     * in screen space, so step it incrementally alongside the edges. */
    float inv_area = 1.0f / area2;
    float z_row = (w0_row * (float)az + w1_row * (float)bz + w2_row * (float)cz) * inv_area;
    float z_dx = (w0_dx * (float)az + w1_dx * (float)bz + w2_dx * (float)cz) * inv_area;
    float z_dy = (w0_dy * (float)az + w1_dy * (float)bz + w2_dy * (float)cz) * inv_area;

    for (int py = min_y; py <= max_y; py++) {
        float w0 = w0_row, w1 = w1_row, w2 = w2_row, z = z_row;
        uint16_t *fb_row = s_fb + py * R3D_FB_W;
        uint16_t *dp_row = s_depth + py * R3D_FB_W;
        for (int px = min_x; px <= max_x; px++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                uint16_t d = (uint16_t)z;
                if (d > dp_row[px]) {
                    if (write) dp_row[px] = d;
                    fb_row[px] = color;
                }
            }
            w0 += w0_dx; w1 += w1_dx; w2 += w2_dx; z += z_dx;
        }
        w0_row += w0_dy; w1_row += w1_dy; w2_row += w2_dy; z_row += z_dy;
    }
}

void r3d_tri(float ax, float ay, uint16_t az, float bx, float by, uint16_t bz,
             float cx, float cy, uint16_t cz, uint16_t color, int y_min, int y_max) {
    tri_raster(ax,ay,az, bx,by,bz, cx,cy,cz, color, y_min, y_max, 1);   /* writes depth */
}
/* Depth-TESTED but NOT depth-writing (pocket lips): cushions occlude them, yet
 * the balls drawn afterwards always cover them. */
void r3d_tri_nowrite(float ax, float ay, uint16_t az, float bx, float by, uint16_t bz,
                     float cx, float cy, uint16_t cz, uint16_t color, int y_min, int y_max) {
    tri_raster(ax,ay,az, bx,by,bz, cx,cy,cz, color, y_min, y_max, 0);
}
