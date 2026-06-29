#ifndef ROGUE_DMGNUM_H
#define ROGUE_DMGNUM_H
/*
 * ThumbyRogue floating damage numbers — small text motes that rise from an
 * impact and fade. Green = damage you deal, red = damage you take. A tiny
 * fixed pool; spawned at the point damage is actually applied.
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "craft_render.h"

void rogue_dmgnum_clear(void);
/* Spawn a number at a world point. taken=true -> red (you were hurt),
 * taken=false -> green (you dealt it). Values <= 0 are ignored. */
void rogue_dmgnum_spawn(Vec3 pos, int value, bool taken);
void rogue_dmgnum_update(float dt);
void rogue_dmgnum_draw(const CraftCamera *cam, uint16_t *fb);

#endif /* ROGUE_DMGNUM_H */
