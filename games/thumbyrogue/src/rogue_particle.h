#ifndef ROGUE_PARTICLE_H
#define ROGUE_PARTICLE_H
/*
 * ThumbyRogue particle system — a small pool of cuboid sparks/motes with
 * velocity, gravity and fade. Reused for melee hit sparks, enemy death poofs,
 * projectile trails + impact bursts, and pickup sparkles.
 */
#include <stdint.h>
#include "craft_types.h"
#include "craft_render.h"

void rogue_particle_clear(void);
void rogue_particle_spawn(Vec3 pos, float vx, float vy, float vz,
                          float life, uint16_t col, float size, float grav);
/* Spawn `n` motes flying outward from `pos`. */
void rogue_particle_burst(Vec3 pos, int n, float speed, float life,
                          uint16_t col, float size);
void rogue_particle_update(float dt);
void rogue_particle_draw(const CraftCamera *cam, uint16_t *fb);

#endif /* ROGUE_PARTICLE_H */
