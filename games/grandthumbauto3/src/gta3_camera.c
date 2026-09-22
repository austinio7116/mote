#include "gta3_camera.h"

#include <math.h>

void gta3_cam_reset(Gta3Cam *c) {
    c->eye = v3(0, 0, 0);
    c->target = v3(0, 0, 0);
    c->yaw = 0.0f;
    c->started = 0;
}

float gta3_wrap_angle(float a) {
    const float TAU = 6.2831853f;
    while (a >  3.14159265f) a -= TAU;
    while (a <= -3.14159265f) a += TAU;
    return a;
}

/* March the tile grid from the anchor toward the wanted eye and report how far
 * we may travel before hitting something solid. Steps at a quarter tile: the
 * grid is 4 m and buildings are full tiles, so this cannot skip a wall, and at
 * a maximum of ~10 m of travel it is at most 40 samples. */
static float clear_run(float ax, float az, float dx, float dz, float want,
                       Gta3SolidFn solid, void *ud, float tile_m) {
    if (!solid) return want;
    float step = tile_m * 0.25f;
    for (float t = step; t <= want; t += step) {
        int tx = (int)floorf((ax + dx * t) / tile_m);
        int tz = (int)floorf((az + dz * t) / tile_m);
        if (solid(tx, tz, ud)) {
            /* Back off to the LAST KNOWN-GOOD sample, minus a small skin. Half a
             * step back from the hit sample is not enough — the wall face can lie
             * anywhere between the last good sample and this one, so half a step
             * can still leave the eye inside the building. */
            float back = (t - step) - 0.15f;
            return back < 0.0f ? 0.0f : back;
        }
    }
    return want;
}

void gta3_cam_update(Gta3Cam *c, float anchor_x, float anchor_z, float facing_yaw,
                     float dist, float height, float look, float dt,
                     Gta3SolidFn solid, void *ud, float tile_m) {
    /* Yaw is smoothed, not the eye vector, so the camera swings through corners
     * rather than sliding sideways through them. */
    if (!c->started) {
        c->yaw = facing_yaw;
    } else {
        float d = gta3_wrap_angle(facing_yaw - c->yaw);
        float ky = 1.0f - expf(-GTA3_CAM_YAW_K * dt);
        c->yaw = gta3_wrap_angle(c->yaw + d * ky);
    }

    float fx = cosf(c->yaw), fz = sinf(c->yaw);

    /* How far back may we actually sit? Cast from the anchor along -fwd. */
    float run = clear_run(anchor_x, anchor_z, -fx, -fz, dist, solid, ud, tile_m);
    if (run < GTA3_CAM_MIN_D) run = GTA3_CAM_MIN_D;

    Vec3 want_eye = v3(anchor_x - fx * run, height, anchor_z - fz * run);
    Vec3 want_tgt = v3(anchor_x + fx * look, GTA3_CAM_EYE_Y, anchor_z + fz * look);

    if (!c->started) {
        c->eye = want_eye;
        c->target = want_tgt;
        c->started = 1;
        return;
    }

    float kp = 1.0f - expf(-GTA3_CAM_POS_K * dt);
    c->eye = v3_lerp(c->eye, want_eye, kp);
    c->target = v3_lerp(c->target, want_tgt, kp);
}
