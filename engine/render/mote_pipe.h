/*
 * Mote — 3D geometry pipeline. (Ported from ThumbyElite r3d_pipe.)
 *
 * model -> world -> view -> near-clip -> project -> mote_tri.
 *
 * CAMERA-RELATIVE WORLD: the camera is always the origin. Callers pass object
 * positions already relative to the camera. This keeps every float the pipe
 * touches small even across huge worlds.
 *
 * View space: x right, y up, z forward (depth). Projection flips y for the
 * y-down screen. Depth value d = MOTE_DEPTH_K / z (larger = nearer).
 */
#ifndef MOTE_PIPE_H
#define MOTE_PIPE_H

#include "../math/mote_vec.h"
#include "../assets/mote_mesh.h"
#include "../core/mote_config.h"
#include "mote_object.h"
#include <stdint.h>

void mote_pipe_set_camera(const Mat3 *cam_basis, float fov_deg);
void mote_pipe_set_camera_pos(Vec3 cam_pos);   /* opt into absolute world positions */
Vec3 mote_pipe_cam_pos(void);
void mote_pipe_set_sun(Vec3 dir_toward_light_world);
void mote_pipe_set_near(float near_m);   /* runtime near plane (default MOTE_NEAR) */
float mote_pipe_near(void);
float mote_pipe_depth_k(void);

/* Transform, light, clip and project one object, emitting final screen
 * triangles via mote_emit_tri (implemented by mote_scene). Returns tri count. */
int mote_pipe_draw_object(const MoteObject *obj);
int mote_pipe_draw_object_scaled(const MoteObject *obj, float s);

/* Implemented by mote_scene3d.c — appends a screen triangle to the draw-list. */
void mote_emit_tri(float ax, float ay, uint16_t az,
                 float bx, float by, uint16_t bz,
                 float cx, float cy, uint16_t cz, uint16_t color);
/* Implemented by mote_scene3d.c — appends a TEXTURED (UV-mapped) screen tri. */
void mote_emit_textri(float ax, float ay, uint16_t az, float au, float av,
                      float bx, float by, uint16_t bz, float bu, float bv,
                      float cx, float cy, uint16_t cz, float cu, float cv,
                      const MoteImage *tex, uint8_t shade);

const Mat3 *mote_pipe_camera(void);
float mote_pipe_focal(void);
Vec3 mote_pipe_sun_view(void);
Vec3 mote_pipe_sun_world(void);

#endif /* MOTE_PIPE_H */
