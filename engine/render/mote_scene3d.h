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

#define MOTE_SCENE_MAX_TRIS 3328   /* raised from 2048 (freed the 32KB panic fb) */
#define MOTE_SCENE_MAX_SPHERES 200

void mote_scene_set_background(uint16_t rgb565);

void mote_scene_begin(const Mat3 *cam_basis, float fov_deg);
void mote_scene_clear(void);   /* drop the draw-list, keep the camera */

/* Returns triangles added (0 if culled or the list is full). */
int mote_scene_add_object(const MoteObject *obj);
int mote_scene_add_object_scaled(const MoteObject *obj, float scale);

/* A per-pixel shaded sphere impostor at a camera-relative world position. */
int mote_scene_add_sphere(Vec3 cam_rel_pos, float radius, uint16_t color);

/* Rasterise LOGICAL rows [y0, y1): clears that physical band of fb + depth,
 * then every listed triangle clamped to the band. Safe to call concurrently
 * from both cores with disjoint bands. */
void mote_scene_raster(uint16_t *fb, int y0, int y1);

int mote_scene_tri_count(void);

#endif /* MOTE_SCENE3D_H */
