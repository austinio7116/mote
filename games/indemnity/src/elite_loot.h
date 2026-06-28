/*
 * ThumbyElite — wreck salvage: canisters, scooping.
 */
#ifndef ELITE_LOOT_H
#define ELITE_LOOT_H

#include "vec.h"
#include "elite_entity.h"
#include <stdint.h>

void loot_init(void);
void loot_seed(uint32_t seed);   /* per-run stream (new game / load) */
/* Roll a drop at a kill site (tier raises component odds/quality). */
void loot_on_kill(Vec3 pos, Vec3 vel, int tier,
                  const Ship *victim);
/* Tumble + scoop check. Returns a toast string for this frame or NULL. */
const char *loot_tick(float dt);
/* Add live canisters to the scene (camera-relative). */
void loot_render(Vec3 cam_pos);
/* Drop beacons (point + light-mast); false = bare cubes (title intro). */
void loot_set_beacons(bool on);

/* Scanner support: fill out[] with live canister positions (and
 * whether each holds a component). Returns the count. */
int loot_positions(Vec3 *out, int *is_component, int max);
/* Nearest live canister (LB lock fallback). -1 if none. */
int loot_nearest(Vec3 from, Vec3 *out_pos);
/* Tractor beam: pull canisters within range toward a point. */
void loot_tractor_pull(Vec3 to, float range, float speed);
/* Mining: spill an ore canister (minerals/metals, rare gems). */
void loot_spawn_ore(Vec3 pos, Vec3 vel);
/* Spill a canister of a specific good (civilian wreck cargo). */
void loot_spawn_good(Vec3 pos, Vec3 vel, int good, int count);

#endif
