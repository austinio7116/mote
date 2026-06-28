/*
 * Indemnity Run — camera state for the SKY background projection.
 *
 * The full geometry pipeline is GONE — the Mote engine transforms, clips,
 * projects and rasterises everything the game submits (scene_add_object etc).
 * What survives is a tiny camera holder the background painter (starfield +
 * galaxies in r3d_scene.c) uses to project fixed sky directions, since the
 * engine's own camera isn't exposed through the ABI. The game sets it each
 * frame alongside scene_begin.
 */
#ifndef R3D_PIPE_H
#define R3D_PIPE_H

#include "vec.h"
#include <stdint.h>

#define R3D_NEAR    0.5f                      /* near plane, metres (== engine default) */
#define R3D_DEPTH_K (65535.0f * R3D_NEAR)     /* depth d = K/z, matches the engine */

void        r3d_pipe_set_camera(const Mat3 *cam_basis, float fov_deg);
const Mat3 *r3d_pipe_camera(void);
float       r3d_pipe_focal(void);

/* Project a CAMERA-RELATIVE world position to logical screen + depth (for HUD
 * reticles / markers). Returns 0 if behind the near plane. */
int r3d_pipe_project(Vec3 cam_rel, float *sx, float *sy, uint16_t *d);

#endif
