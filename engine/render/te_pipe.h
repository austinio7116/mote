/*
 * ThumbyEngine — 3D geometry pipeline. (Ported from ThumbyElite r3d_pipe.)
 *
 * model -> world -> view -> near-clip -> project -> te_tri.
 *
 * CAMERA-RELATIVE WORLD: the camera is always the origin. Callers pass object
 * positions already relative to the camera. This keeps every float the pipe
 * touches small even across huge worlds.
 *
 * View space: x right, y up, z forward (depth). Projection flips y for the
 * y-down screen. Depth value d = TE_DEPTH_K / z (larger = nearer).
 */
#ifndef TE_PIPE_H
#define TE_PIPE_H

#include "../math/te_vec.h"
#include "../assets/te_mesh.h"
#include "../core/te_config.h"
#include "te_object.h"
#include <stdint.h>

void te_pipe_set_camera(const Mat3 *cam_basis, float fov_deg);
void te_pipe_set_sun(Vec3 dir_toward_light_world);

/* Transform, light, clip and project one object, emitting final screen
 * triangles via te_emit_tri (implemented by te_scene). Returns tri count. */
int te_pipe_draw_object(const TeObject *obj);
int te_pipe_draw_object_scaled(const TeObject *obj, float s);

/* Implemented by te_scene3d.c — appends a screen triangle to the draw-list. */
void te_emit_tri(float ax, float ay, uint16_t az,
                 float bx, float by, uint16_t bz,
                 float cx, float cy, uint16_t cz, uint16_t color);

const Mat3 *te_pipe_camera(void);
float te_pipe_focal(void);

#endif /* TE_PIPE_H */
