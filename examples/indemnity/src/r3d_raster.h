/*
 * ThumbyElite — flat-shaded depth-tested triangle rasterizer.
 *
 * The hot loop of the renderer. Screen-space triangles only — the geometry
 * pipe (r3d_pipe) has already transformed, near-clipped, projected and
 * lit them. Incremental edge functions + a depth plane equation keep the
 * inner loop to a handful of FPU adds per pixel.
 *
 * Depth convention: uint16, LARGER = NEARER (d = K/z). The buffer is
 * cleared to 0 each frame; pixel writes require d > depth[i].
 *
 * y_min/y_max clamp the rasterised rows — this is the dual-core split:
 * both cores walk the same draw-list, core0 with rows [0,64) and core1
 * with rows [64,128). Disjoint pixels, no locks.
 */
#ifndef R3D_RASTER_H
#define R3D_RASTER_H

#include <stdint.h>
#include "elite_types.h"

void r3d_raster_set_fb(uint16_t *fb);
void r3d_depth_clear(int y_min, int y_max);

/* Direct depth-buffer access for renderers with their own pixel loops
 * (planet impostors). Layout: u16[R3D_FB_W * R3D_FB_H]. NOTE: everything
 * in this module — coordinates, radii AND y_min/y_max bands — is in
 * PHYSICAL pixels (logical x R3D_SS); r3d_scene_raster does the scaling. */
uint16_t *r3d_depth_buffer(void);

/* Screen-space flat triangle. (ax,ay,az)... are screen x/y (subpixel
 * floats) and the uint16 depth value at each vertex. Vertices must wind
 * clockwise on screen; counter-clockwise triangles are culled (backface). */
void r3d_tri(float ax, float ay, uint16_t az,
             float bx, float by, uint16_t bz,
             float cx, float cy, uint16_t cz,
             uint16_t color, int y_min, int y_max);

/* Depth-TESTED (but not depth-writing) primitives for particles/beams —
 * drawn after the triangle pass so ships correctly occlude them. */
void r3d_point(int x, int y, uint16_t d, uint16_t color, int size,
               int y_min, int y_max);
void r3d_line(float x0, float y0, uint16_t d0,
              float x1, float y1, uint16_t d1,
              uint16_t color, int y_min, int y_max);

/* Filled circle, depth-tested per pixel (explosion fireballs, flashes). */
void r3d_disc(int cx, int cy, uint16_t d, int r, uint16_t color,
              int y_min, int y_max);

/* Mote: the depth buffer lives in the load-time arena. Alloc r3d_depth_bytes()
 * and hand it in with r3d_set_depth() before the first render. */
#include <stddef.h>
size_t r3d_depth_bytes(void);
void   r3d_set_depth(uint16_t *d);

#endif
