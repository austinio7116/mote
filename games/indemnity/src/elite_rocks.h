/*
 * ThumbyElite — asteroid fields + mining.
 */
#ifndef ELITE_ROCKS_H
#define ELITE_ROCKS_H

#include "vec.h"
#include <stdint.h>
#include <stdbool.h>

void rocks_init(void);
void rocks_spawn_field(uint32_t seed, int n);
void rocks_tick(float dt);
void rocks_render(Vec3 cam_pos, float t);
/* Hitscan ray vs rocks: index or -1, hit distance via t_out. */
int  rocks_ray(Vec3 o, Vec3 dir, float max_t, float *t_out);
/* Chip a rock; yield_mult scales ore recovery (mining laser 1.0,
 * crude weapons ~0.45 — blasting vaporizes ore). True if destroyed. */
bool rocks_damage(int idx, float dmg, float yield_mult, Vec3 hit_pos);
int  rocks_positions(Vec3 *out, int max);
int  rocks_get(int idx, Vec3 *pos, float *radius);   /* alive? */

#endif
