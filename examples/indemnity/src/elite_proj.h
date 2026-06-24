/*
 * ThumbyElite — projectile pool (photons, slugs, tracers, missiles).
 */
#ifndef ELITE_PROJ_H
#define ELITE_PROJ_H

#include "vec.h"
#include "elite_weapons.h"
#include <stdint.h>
#include <stdbool.h>

void proj_init(void);
void proj_spawn(WeaponType type, int owner, int8_t target,
                Vec3 pos, Vec3 dir, Vec3 inherit_vel);
/* Proximity mine: stationary, 18m trigger, 25s life. */
void proj_spawn_mine(int owner, Vec3 pos, Vec3 vel, float dmg_mult);
/* Chaff: every seeker tracking `victim` loses its lock. Returns count. */
int proj_break_locks(int victim);
float proj_nearest_homing(int victim);
Vec3 proj_homing_pos(int victim);
/* Is any live seeker tracking this ship? (missile warning) */
bool proj_homing_on(int victim);
void proj_spawn_ex(WeaponType type, int owner, int8_t target,
                   Vec3 pos, Vec3 dir, Vec3 inherit_vel, float dmg_mult);
void proj_tick(float dt);
/* Project live rounds into the scene (camera-relative). */
void proj_emit(Vec3 cam_pos);
int  proj_count(void);
void proj_clear_all(void);

#endif
