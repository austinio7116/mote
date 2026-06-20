/*
 * Mote — flat-shaded depth-tested triangle rasterizer.
 * (Ported from ThumbyElite r3d_raster.)
 *
 * Screen-space triangles only — the geometry pipe (mote_pipe) has already
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
 * PHYSICAL pixels (logical x MOTE_SS); mote_scene scales before calling in.
 */
#ifndef MOTE_RASTER_H
#define MOTE_RASTER_H

#include <stdint.h>
#include "../core/mote_config.h"

void mote_raster_set_fb(uint16_t *fb);
void mote_depth_clear(int y_min, int y_max);
uint16_t *mote_depth_buffer(void);

/* Screen-space flat triangle. (ax,ay,az)... are screen x/y (subpixel floats)
 * and the uint16 depth value at each vertex. Vertices must wind clockwise on
 * screen; counter-clockwise triangles are culled (backface). */
void mote_tri(float ax, float ay, uint16_t az,
            float bx, float by, uint16_t bz,
            float cx, float cy, uint16_t cz,
            uint16_t color, int y_min, int y_max);

/* Depth-TESTED (but not depth-writing) primitives for particles/beams. */
void mote_point(int x, int y, uint16_t d, uint16_t color, int size,
              int y_min, int y_max);
void mote_line(float x0, float y0, uint16_t d0,
             float x1, float y1, uint16_t d1,
             uint16_t color, int y_min, int y_max);
void mote_disc(int cx, int cy, uint16_t d, int r, uint16_t color,
             int y_min, int y_max);

/* Allocate the depth buffer from the load-time arena (only when the game does
 * 3D). want_depth==0 leaves it NULL and depth ops become no-ops. */
struct MoteArena;
int mote_raster_configure(struct MoteArena *arena, int want_depth);

#endif /* MOTE_RASTER_H */
