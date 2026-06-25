/*
 * Mote — flat-shaded depth-tested triangle rasterizer.
 * (Ported from ThumbyElite r3d_raster.c.)
 *
 * Technique: half-space (edge function) rasterisation with incremental
 * row/column stepping (Fabian Giesen's optimised basic rasteriser), flat
 * colour only, with depth interpolated by plane equation instead of
 * per-pixel barycentrics, so the inner loop is 4 fadds + 2 compares.
 */
#include "mote_raster.h"
#include "mote_arena.h"
#include <math.h>
#include <string.h>

/* All coordinates and row bands in this file are PHYSICAL pixels
 * (MOTE_FB_PW x MOTE_FB_PH). With MOTE_SS == 1 they equal logical space. */

static uint16_t *s_fb;
/* Depth buffer (32 KB) — arena-allocated at load only when the game does 3D
 * (a pure-2D game pays nothing). Both cores read/write disjoint row bands. */
static uint16_t *s_depth;

int mote_raster_configure(MoteArena *arena, int want_depth) {
    s_depth = want_depth ? mote_arena_alloc(arena, (size_t)MOTE_FB_PW * MOTE_FB_PH * sizeof(uint16_t)) : 0;
    return !want_depth || s_depth;
}

void mote_raster_set_fb(uint16_t *fb) { s_fb = fb; }
uint16_t *mote_depth_buffer(void) { return s_depth; }

void mote_depth_clear(int y_min, int y_max) {
    if (!s_depth) return;
    if (y_min < 0) y_min = 0;
    if (y_max > MOTE_FB_PH) y_max = MOTE_FB_PH;
    if (y_max > y_min)
        memset(s_depth + y_min * MOTE_FB_PW, 0,
               (size_t)(y_max - y_min) * MOTE_FB_PW * sizeof(uint16_t));
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void mote_point(int x, int y, uint16_t d, uint16_t color, int size,
              int y_min, int y_max) {
    for (int dy = 0; dy < size; dy++) {
        int py = y + dy;
        if (py < y_min || py >= y_max) continue;
        uint16_t *fb_row = s_fb + py * MOTE_FB_PW;
        uint16_t *dp_row = s_depth + py * MOTE_FB_PW;
        for (int dx = 0; dx < size; dx++) {
            int px = x + dx;
            if ((unsigned)px >= MOTE_FB_PW) continue;
            if (d > dp_row[px]) fb_row[px] = color;
        }
    }
}

void mote_disc(int cx, int cy, uint16_t d, int r, uint16_t color,
             int y_min, int y_max) {
    if (r < 1) r = 1;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < y_min || py >= y_max) continue;
        int half = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - half, x1 = cx + half;
        if (x0 < 0) x0 = 0;
        if (x1 > MOTE_FB_PW - 1) x1 = MOTE_FB_PW - 1;
        uint16_t *fb_row = s_fb + py * MOTE_FB_PW;
        uint16_t *dp_row = s_depth + py * MOTE_FB_PW;
        for (int px = x0; px <= x1; px++)
            if (d > dp_row[px]) fb_row[px] = color;
    }
}

void mote_line(float x0, float y0, uint16_t d0,
             float x1, float y1, uint16_t d1,
             uint16_t color, int y_min, int y_max) {
    float dx = x1 - x0, dy = y1 - y0;
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (int)(adx > ady ? adx : ady) + 1;
    if (steps > 256 * MOTE_SS) steps = 256 * MOTE_SS;
    float inv = 1.0f / (float)steps;
    float sx = dx * inv, sy = dy * inv, sd = ((float)d1 - (float)d0) * inv;
    float px = x0, py = y0, pd = (float)d0;
    for (int i = 0; i <= steps; i++) {
        int ix = (int)px, iy = (int)py;
#if MOTE_SS == 1
        if (iy >= y_min && iy < y_max && (unsigned)ix < MOTE_FB_PW) {
            int idx = iy * MOTE_FB_PW + ix;
            if ((uint16_t)pd > s_depth[idx]) s_fb[idx] = color;
        }
#else
        for (int by = 0; by < MOTE_SS; by++) {
            int yy = iy + by;
            if (yy < y_min || yy >= y_max) continue;
            for (int bx = 0; bx < MOTE_SS; bx++) {
                int xx = ix + bx;
                if ((unsigned)xx >= MOTE_FB_PW) continue;
                int idx = yy * MOTE_FB_PW + xx;
                if ((uint16_t)pd > s_depth[idx]) s_fb[idx] = color;
            }
        }
#endif
        px += sx; py += sy; pd += sd;
    }
}

/* Shared core; write_depth is a literal at both call sites so -O2 folds the
 * inner branch away — mote_tri and mote_tri_nowrite each get a clean hot loop. */
static inline __attribute__((always_inline))
void tri_core(float ax, float ay, uint16_t az,
            float bx, float by, uint16_t bz,
            float cx, float cy, uint16_t cz,
            uint16_t color, int y_min, int y_max, int write_depth) {
    /* Signed area*2. Screen-clockwise => positive. <=0 is backfacing or
     * degenerate — cull. */
    float area2 = edge(ax, ay, bx, by, cx, cy);
    if (area2 <= 0.0f) return;

    float fminx = ax < bx ? ax : bx; if (cx < fminx) fminx = cx;
    float fminy = ay < by ? ay : by; if (cy < fminy) fminy = cy;
    float fmaxx = ax > bx ? ax : bx; if (cx > fmaxx) fmaxx = cx;
    float fmaxy = ay > by ? ay : by; if (cy > fmaxy) fmaxy = cy;

    int min_x = (int)fminx; if (min_x < 0) min_x = 0;
    int min_y = (int)fminy; if (min_y < y_min) min_y = y_min;
    int max_x = (int)fmaxx; if (max_x > MOTE_FB_PW - 1) max_x = MOTE_FB_PW - 1;
    int max_y = (int)fmaxy; if (max_y >= y_max) max_y = y_max - 1;
    if (min_x > max_x || min_y > max_y) return;

    float px0 = (float)min_x + 0.5f, py0 = (float)min_y + 0.5f;
    float w0_row = edge(bx, by, cx, cy, px0, py0);   /* weight of A */
    float w1_row = edge(cx, cy, ax, ay, px0, py0);   /* weight of B */
    float w2_row = edge(ax, ay, bx, by, px0, py0);   /* weight of C */

    float w0_dx = by - cy, w0_dy = cx - bx;
    float w1_dx = cy - ay, w1_dy = ax - cx;
    float w2_dx = ay - by, w2_dy = bx - ax;

    float inv_area = 1.0f / area2;
    float z_row = (w0_row * (float)az + w1_row * (float)bz + w2_row * (float)cz) * inv_area;
    float z_dx = (w0_dx * (float)az + w1_dx * (float)bz + w2_dx * (float)cz) * inv_area;
    float z_dy = (w0_dy * (float)az + w1_dy * (float)bz + w2_dy * (float)cz) * inv_area;

    for (int py = min_y; py <= max_y; py++) {
        float w0 = w0_row, w1 = w1_row, w2 = w2_row, z = z_row;
        uint16_t *fb_row = s_fb + py * MOTE_FB_PW;
        uint16_t *dp_row = s_depth + py * MOTE_FB_PW;
        for (int px = min_x; px <= max_x; px++) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                uint16_t d = (uint16_t)z;
                if (d > dp_row[px]) {
                    if (write_depth) dp_row[px] = d;
                    fb_row[px] = color;
                }
            }
            w0 += w0_dx; w1 += w1_dx; w2 += w2_dx; z += z_dx;
        }
        w0_row += w0_dy; w1_row += w1_dy; w2_row += w2_dy; z_row += z_dy;
    }
}

MOTE_HOT
void mote_tri(float ax, float ay, uint16_t az,
            float bx, float by, uint16_t bz,
            float cx, float cy, uint16_t cz,
            uint16_t color, int y_min, int y_max) {
    tri_core(ax, ay, az, bx, by, bz, cx, cy, cz, color, y_min, y_max, 1);
}

MOTE_HOT
void mote_tri_nowrite(float ax, float ay, uint16_t az,
            float bx, float by, uint16_t bz,
            float cx, float cy, uint16_t cz,
            uint16_t color, int y_min, int y_max) {
    tri_core(ax, ay, az, bx, by, bz, cx, cy, cz, color, y_min, y_max, 0);
}
