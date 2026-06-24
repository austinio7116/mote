/*
 * ThumbyElite — 3D geometry pipeline.
 *
 * model -> world -> view -> near-clip -> project -> r3d_tri.
 *
 * CAMERA-RELATIVE WORLD: the camera is always the origin. Callers pass
 * object positions already relative to the camera (game code subtracts the
 * camera's logical position before drawing). This keeps every float the
 * pipe touches small even though systems span millions of km.
 *
 * View space: x right, y up, z forward (depth). Projection flips y for
 * the y-down screen. Depth buffer value d = R3D_DEPTH_K / z (larger =
 * nearer), matching r3d_raster's convention.
 */
#ifndef R3D_PIPE_H
#define R3D_PIPE_H

#include "vec.h"
#include "r3d_mesh.h"
#include <stdint.h>

#define R3D_NEAR     0.5f                      /* near plane, meters */
#define R3D_DEPTH_K  (65535.0f * R3D_NEAR)     /* d=65535 at z=NEAR */
#define R3D_MAX_VERTS 320                      /* largest mesh (station) */

typedef struct {
    Vec3 pos;          /* camera-relative world position */
    Mat3 basis;        /* object orientation (rows: right/up/forward) */
    const Mesh *mesh;
} R3DObject;

void r3d_pipe_set_camera(const Mat3 *cam_basis, float fov_deg);
void r3d_pipe_set_sun(Vec3 dir_toward_light_world);

/* Transform, light, clip and project one object, emitting final screen
 * triangles via r3d_emit_tri (implemented by r3d_scene — appends to the
 * frame draw-list). Returns the number of triangles emitted. */
int r3d_pipe_draw_object(const R3DObject *obj);
/* Per-instance uniform scale (rocks): verts and bound scale by s; the
 * basis stays orthonormal so lighting and culling stay correct. */
int r3d_pipe_draw_object_scaled(const R3DObject *obj, float s);

/* Implemented by r3d_scene.c. */
void r3d_emit_tri(float ax, float ay, uint16_t az,
                  float bx, float by, uint16_t bz,
                  float cx, float cy, uint16_t cz, uint16_t color);

/* Camera basis / focal length as last set (starfield + HUD projections). */
const Mat3 *r3d_pipe_camera(void);
float r3d_pipe_focal(void);

#endif
