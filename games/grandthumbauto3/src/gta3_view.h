/*
 * Grand Thumb Auto III — world-space visibility.
 *
 * Top-down, deciding what to draw was a fixed square of tiles around the player
 * and a screen-space test per tile. Behind a car that is both too small (the
 * world would end mid-street) and too wasteful (most of the square is behind
 * you). This is the replacement: a cone from the eye along the camera forward,
 * with a per-caller distance cap so ground and buildings can use different
 * draw distances.
 *
 * Pure maths. No engine, no renderer, no globals — so it is testable on the host.
 */
#ifndef GTA3_VIEW_H
#define GTA3_VIEW_H

#include "mote_vec.h"

typedef struct {
    Vec3  eye;       /* camera position, world metres */
    Vec3  fwd;       /* camera forward, unit length */
    float cos_half;  /* cosine of the half-angle the cone accepts */
    float sin_half;  /* sine of the same angle — frame-invariant, so it is
                      * computed once here rather than per tile test */
} Gta3View;

/* Build the cone for this frame.
 *
 * `fov_deg` is the engine's vertical field of view — the same value handed to
 * scene_camera(). The screen is square (128x128), so horizontal and vertical
 * are equal, and the corner of the frustum sits further out than either: the
 * half-angle is scaled by `aspect_slack` (use 1.45f, ~sqrt(2) plus margin) so
 * tiles at the screen corners are not culled. */
void gta3_view_set(Gta3View *v, Vec3 eye, Vec3 fwd, float fov_deg, float aspect_slack);

/* Is the tile centred at (wx, wy, wz) worth submitting?
 *
 * Rejects beyond `radius_m`, then outside the cone. The angular threshold is
 * slackened by the tile's own angular size (a tile of `tile_m` at that
 * distance), so a tile whose CENTRE is outside the cone but whose near corner
 * is inside still draws — without that, tiles pop at the screen edge.
 *
 * Returns 1 to draw, 0 to skip. */
int gta3_view_tile(const Gta3View *v, float wx, float wy, float wz,
                   float radius_m, float tile_m);

#endif /* GTA3_VIEW_H */
