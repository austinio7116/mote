/*
 * Grand Thumb Auto III — the chase camera.
 *
 * Three jobs, and each exists for a reason that shows immediately if it is
 * skipped:
 *
 *  · FRAMING — eye behind and above the anchor, target ahead of it. The look
 *    ahead distance is what keeps the road, rather than the roof of your own
 *    car, in the middle of a 128x128 screen.
 *
 *  · SMOOTHING — position and yaw are smoothed at DIFFERENT rates, yaw slower.
 *    Equal rates make corners snap; a slower yaw is what makes the camera swing
 *    through a turn. Yaw follows the vehicle's HEADING, never its velocity, so
 *    reversing does not spin the camera around.
 *
 *  · COLLISION — a DDA along the tile grid from the anchor back to the wanted
 *    eye. In a grid city, every corner would otherwise put the camera inside a
 *    facade and show you the inside of a building. This is not optional.
 *
 * Pure maths: the tile map arrives as a callback, so this is testable on the
 * host with a hand-written map and no engine.
 */
#ifndef GTA3_CAMERA_H
#define GTA3_CAMERA_H

#include "mote_vec.h"

typedef struct {
    Vec3  eye;      /* smoothed camera position, world metres */
    Vec3  target;   /* smoothed look-at point, world metres */
    float yaw;      /* smoothed camera yaw, radians */
    int   started;  /* 0 until the first update snaps instead of smoothing */
} Gta3Cam;

/* Returns non-zero if the tile at (tx, tz) blocks the camera. */
typedef int (*Gta3SolidFn)(int tx, int tz, void *ud);

/* Forget the smoothed state. Call on respawn, on entering or leaving a car, and
 * at the start of a game — otherwise the camera sails across the map from
 * wherever it was. */
void gta3_cam_reset(Gta3Cam *c);

/* Wrap to (-pi, pi]. Exposed because yaw smoothing must go the short way round
 * and callers need the same wrap for their own heading maths. */
float gta3_wrap_angle(float a);

/* Advance the camera one frame.
 *
 * `anchor_x`/`anchor_z` are the player or vehicle position; `facing_yaw` is its
 * heading. `dist`, `height` and `look` are the framing for the current state
 * (on foot or in a car at this speed). `solid`/`ud` answer which tiles block,
 * and `tile_m` is the world size of one tile. */
void gta3_cam_update(Gta3Cam *c, float anchor_x, float anchor_z, float facing_yaw,
                     float dist, float height, float look, float dt,
                     Gta3SolidFn solid, void *ud, float tile_m);

/* The same, for an anchor that is OFF THE GROUND.
 *
 * gta3_cam_update treats `height` as an absolute eye y and looks at the fixed
 * GTA3_CAM_LOOK_Y, both of which assume the thing being followed is standing on
 * the road. A helicopter at 40 m would be framed from 2.8 m, i.e. not in shot at
 * all. Here `anchor_y` lifts both: the eye sits at anchor_y + height and the
 * target at anchor_y + GTA3_CAM_LOOK_Y.
 *
 * Pass `solid = 0` when the anchor is above the rooftops. The collision DDA
 * walks the tile grid with no notion of height, so a camera flying over a block
 * is otherwise shoved forward by a building it is nowhere near.
 *
 * gta3_cam_update is exactly this with anchor_y = 0. */
void gta3_cam_update_y(Gta3Cam *c, float anchor_x, float anchor_y, float anchor_z,
                       float facing_yaw, float dist, float height, float look,
                       float dt, Gta3SolidFn solid, void *ud, float tile_m);

/* Smoothing rates, per second. Position is faster than yaw on purpose. */
#define GTA3_CAM_POS_K   8.0f
#define GTA3_CAM_YAW_K   4.0f
/* The camera never comes closer to the anchor than this, whatever it hits. */
#define GTA3_CAM_MIN_D   1.8f
/* Height of the look-at TARGET above the anchor's ground position. Not the
 * eye height — that is the caller's `height` argument — and not used by the
 * collision ray, which is purely 2D over the tile grid. */
#define GTA3_CAM_LOOK_Y   1.2f

#endif /* GTA3_CAMERA_H */
