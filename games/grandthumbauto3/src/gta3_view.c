#include "gta3_view.h"

#include <math.h>

void gta3_view_set(Gta3View *v, Vec3 eye, Vec3 fwd, float fov_deg, float aspect_slack) {
    v->eye = eye;
    v->fwd = v3_norm(fwd);
    float half = fov_deg * 0.5f * (3.14159265f / 180.0f) * aspect_slack;
    if (half > 1.55f) half = 1.55f;          /* never open past ~89 deg */
    v->cos_half = cosf(half);
}

int gta3_view_tile(const Gta3View *v, float wx, float wy, float wz,
                   float radius_m, float tile_m) {
    float dx = wx - v->eye.x, dy = wy - v->eye.y, dz = wz - v->eye.z;
    float d2 = dx*dx + dy*dy + dz*dz;
    if (d2 > radius_m * radius_m) return 0;

    /* At or inside a tile's own half-width there is no meaningful direction to
     * test, and this is the ground under the player. Always draw it. */
    float half_tile = tile_m * 0.5f;
    if (d2 <= half_tile * half_tile) return 1;

    float d = sqrtf(d2);
    float cosang = (dx * v->fwd.x + dy * v->fwd.y + dz * v->fwd.z) / d;

    /* Slacken by the tile's angular half-size at this distance: a tile whose
     * centre is outside the cone can still have its near corner on screen.
     * sin(slack) = half_tile / d, and widening the cone by that angle means
     * comparing against cos(half + slack), expanded here rather than via a
     * second acosf/cosf pair. */
    float sin_slack = half_tile / d;
    if (sin_slack > 1.0f) sin_slack = 1.0f;
    float cos_slack = sqrtf(1.0f - sin_slack * sin_slack);
    float sin_half = sqrtf(1.0f - v->cos_half * v->cos_half);
    float cos_widened = v->cos_half * cos_slack - sin_half * sin_slack;

    return cosang >= cos_widened;
}
