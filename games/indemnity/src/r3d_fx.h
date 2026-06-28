/*
 * ThumbyElite — particle / beam effects.
 *
 * One world-space particle pool (engine trails, explosions, sparks) plus
 * short-lived beam segments (laser shots). fx_emit_all() projects live
 * effects into the scene's point/line lists during the frame build.
 */
#ifndef R3D_FX_H
#define R3D_FX_H

#include "vec.h"
#include <stdint.h>

void fx_init(void);
void fx_tick(float dt);

void fx_spawn_explosion(Vec3 pos, Vec3 base_vel);
/* Gauss wake: twin-helix points along [prev..cur], persisting + fading.
 * traveled = total metres flown at cur (phase continuity). */
void fx_gauss_helix(Vec3 prev, Vec3 cur, Vec3 dir, float traveled);
void fx_spawn_crackle(Vec3 pos, Vec3 base_vel, float r);
void fx_spawn_shield_flash(Vec3 pos, Vec3 base_vel, int ion);
void fx_chaff_burst(Vec3 pos, Vec3 base_vel);
uint32_t frnd_pub(void);
void fx_spawn_spark(Vec3 pos, Vec3 base_vel);
void fx_shield_envelope(Vec3 center, Vec3 vel, float radius);
void fx_hull_burst(Vec3 pos, Vec3 vel, float scale);
void fx_break_blast(Vec3 pos, Vec3 vel);
void fx_flak_burst(Vec3 pos, Vec3 base_vel);
/* Per-frame engine trail emission for a thrusting ship. */
void fx_engine_trail(Vec3 rear_pos, Vec3 ship_vel, float throttle, float dt);
/* A laser shot: visible for a few frames. */
void fx_beam(Vec3 from, Vec3 to, uint16_t color);
void fx_lance(Vec3 from, Vec3 to, uint16_t color);

/* Project everything into the scene (camera-relative). Call between
 * r3d_scene_begin and rasterisation, on core0. cam_vel drives the
 * space-dust streaks (sense of speed). */
void fx_emit_all(Vec3 cam_pos, Vec3 cam_vel);

/* Supercruise debris: a wrapping mote field in SYSTEM (Mm) coordinates
 * whose streak length follows the cruise velocity — warp lines at full
 * tilt, drifting sparks near drop speed. */
void fx_sc_dust_emit(Vec3 cam_pos_mm, Vec3 vel_mms);
void fx_sc_dust_off(void);   /* call each frame before building the scene */
void fx_sc_dust_draw(uint16_t *fb, int y0, int y1);   /* drawn by the background pass */

int fx_alive_count(void);

/* Mote: the particle pool lives in the load-time arena. Alloc r3d_fx_parts_bytes()
 * and hand it in with r3d_fx_set_parts() before fx_init(). */
#include <stddef.h>
size_t r3d_fx_parts_bytes(void);
void   r3d_fx_set_parts(void *p);

#endif
