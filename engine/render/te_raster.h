/*
 * ThumbyEngine — flat-shaded depth-tested triangle rasterizer.
 * (Ported from ThumbyElite r3d_raster.)
 *
 * Screen-space triangles only — the geometry pipe (te_pipe) has already
 * transformed, near-clipped, projected and lit them. Incremental edge
 * functions + a depth plane equation keep the inner loop to a handful of
 * FPU adds per pixel.
 *
 * Depth convention: uint16, LARGER = NEARER (d = K/z). The buffer is cleared
 * to 0 each frame; pixel writes require d > depth[i].
 *
 * y_min/y_max clamp the rasterised rows — this is the dual-core split: both
 * cores walk the same draw-list, core0 with rows [0,64) and core1 with rows
 * [64,128). Disjoint pixels, no locks. All coordinates and row bands here are
 * PHYSICAL pixels (logical x TE_SS); te_scene scales before calling in.
 */
#ifndef TE_RASTER_H
#define TE_RASTER_H

#include <stdint.h>
#include "../core/te_config.h"

void te_raster_set_fb(uint16_t *fb);
void te_depth_clear(int y_min, int y_max);
uint16_t *te_depth_buffer(void);

/* Screen-space flat triangle. (ax,ay,az)... are screen x/y (subpixel floats)
 * and the uint16 depth value at each vertex. Vertices must wind clockwise on
 * screen; counter-clockwise triangles are culled (backface). */
void te_tri(float ax, float ay, uint16_t az,
            float bx, float by, uint16_t bz,
            float cx, float cy, uint16_t cz,
            uint16_t color, int y_min, int y_max);

/* Depth-TESTED (but not depth-writing) primitives for particles/beams. */
void te_point(int x, int y, uint16_t d, uint16_t color, int size,
              int y_min, int y_max);
void te_line(float x0, float y0, uint16_t d0,
             float x1, float y1, uint16_t d1,
             uint16_t color, int y_min, int y_max);
void te_disc(int cx, int cy, uint16_t d, int r, uint16_t color,
             int y_min, int y_max);

#endif /* TE_RASTER_H */
