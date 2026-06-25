/*
 * Indemnity Run — camera state for the sky background. See r3d_pipe.h.
 * (The geometry pipeline is the Mote engine's now; this is just the camera the
 * background painter needs to project fixed star/galaxy directions.)
 */
#include "r3d_pipe.h"
#include <math.h>

#define ELITE_FB_W 128

static Mat3  s_cam;
static float s_focal = 64.0f;

void r3d_pipe_set_camera(const Mat3 *cam_basis, float fov_deg) {
    s_cam = *cam_basis;
    s_focal = (ELITE_FB_W * 0.5f) / tanf(fov_deg * (3.14159265f / 180.0f) * 0.5f);
}
const Mat3 *r3d_pipe_camera(void) { return &s_cam; }
float       r3d_pipe_focal(void)  { return s_focal; }

int r3d_pipe_project(Vec3 cam_rel, float *sx, float *sy, uint16_t *d) {
    Vec3 v = m3_mul_v3_t(&s_cam, cam_rel);
    if (v.z <= R3D_NEAR) return 0;
    float inv_z = 1.0f / v.z;
    *sx = (ELITE_FB_W * 0.5f) + s_focal * v.x * inv_z;
    *sy = (ELITE_FB_W * 0.5f) - s_focal * v.y * inv_z;
    float dd = R3D_DEPTH_K * inv_z;
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    return 1;
}
