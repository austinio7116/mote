#ifndef ROGUE_LEVEL_H
#define ROGUE_LEVEL_H
/*
 * ThumbyRogue bounded level. Each floor is generated directly into the
 * static 64^3 craft_world buffer at origin (0,0) — NO sliding window.
 *
 * Phase 1: a single hand-built test room (floor slab + perimeter walls +
 * a few pillars) to exercise the iso camera, movement and collision.
 * Phase 2 replaces the body with a procedural dungeon generator.
 */
#include <stdint.h>
#include "craft_types.h"

#define ROGUE_FLOOR_Y 8   /* world Y the hero stands on (top of floor slab) */

/* Build the test room into craft_world. Returns the hero spawn (feet). */
Vec3 rogue_level_build_test_room(uint32_t seed);

#endif /* ROGUE_LEVEL_H */
