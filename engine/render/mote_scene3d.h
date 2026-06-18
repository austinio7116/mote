/*
 * Mote — scene draw-list + dual-core rasterisation.
 * (Distilled from ThumbyElite r3d_scene — game-specific starfield/nebula
 * dropped; this is the generic engine core.)
 *
 * Frame flow:
 *   core0:  mote_scene_begin(cam, fov)
 *           mote_scene_add_object(...) x N      (transform/clip/shade -> list)
 *           ... then BOTH cores:
 *   coreX:  mote_scene_raster(fb, y0, y1)       (clear band + draw tris)
 *
 * The draw-list holds final screen-space triangles, so rasterisation is
 * embarrassingly parallel: each core walks the whole list clamped to its own
 * row band — disjoint pixels, no locks, no atomics.
 */
#ifndef MOTE_SCENE3D_H
#define MOTE_SCENE3D_H

#include "mote_pipe.h"
#include <stdint.h>

#define MOTE_SCENE_MAX_TRIS 512

void mote_scene_set_background(uint16_t rgb565);

void mote_scene_begin(const Mat3 *cam_basis, float fov_deg);

/* Returns triangles added (0 if culled or the list is full). */
int mote_scene_add_object(const MoteObject *obj);
int mote_scene_add_object_scaled(const MoteObject *obj, float scale);

/* Rasterise LOGICAL rows [y0, y1): clears that physical band of fb + depth,
 * then every listed triangle clamped to the band. Safe to call concurrently
 * from both cores with disjoint bands. */
void mote_scene_raster(uint16_t *fb, int y0, int y1);

int mote_scene_tri_count(void);

#endif /* MOTE_SCENE3D_H */
