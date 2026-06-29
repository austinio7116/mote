#ifndef ROGUE_CAMERA_H
#define ROGUE_CAMERA_H
/*
 * ThumbyRogue isometric follow-camera.
 *
 * Fixed downward pitch (3/4 iso tilt), narrow FOV (near-orthographic look).
 * Yaw snaps to 4 cardinal directions (90° steps, tweened) so the player can
 * peek around walls with LB/RB. The camera looks AT a smoothed focus point
 * that lerps toward the player, and sits behind+above it along the view dir.
 */
#include "craft_types.h"
#include "craft_render.h"

void  rogue_camera_init(Vec3 target);       /* snap focus + reset yaw */
void  rogue_camera_follow(Vec3 target, float dt); /* lerp focus toward target */
void  rogue_camera_rotate(int dir);         /* +1 / -1: snap yaw ±90° */
int   rogue_camera_yaw_index(void);         /* 0..3 cardinal */
float rogue_camera_yaw(void);               /* current animated yaw (rad) */
float rogue_camera_snapped_yaw(void);       /* target yaw for movement remap */
void  rogue_camera_update(float dt);        /* advance yaw tween */
void  rogue_camera_rotate_smooth(float d);  /* LB/RB continuous swivel */
void  rogue_camera_get(CraftCamera *out);   /* fill a CraftCamera to render */

#endif /* ROGUE_CAMERA_H */
