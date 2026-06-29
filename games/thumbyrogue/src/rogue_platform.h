#ifndef ROGUE_PLATFORM_H
#define ROGUE_PLATFORM_H
/*
 * ThumbyRogue moving platforms — solid slabs that oscillate between two
 * points (ferries across lava pits / gaps). They're dynamic, so they live
 * here rather than in the static voxel world; the player's vertical
 * collision queries rogue_platform_support() and is carried by the slab it
 * stands on.
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "craft_render.h"

void rogue_platform_clear(void);
void rogue_platform_place(const int16_t *room_cx, const int16_t *room_cz,
                          int n_rooms, int up_x, int up_z,
                          int floor_y, int depth, uint32_t seed,
                          const int16_t *chasm_x, const int16_t *chasm_z,
                          int n_chasm);
void rogue_platform_update(float dt);

/* If a platform top is within landing reach under the footprint at (x,z,feet),
 * returns true with its top Y and this-frame movement delta (to carry the
 * rider). */
bool rogue_platform_support(float x, float z, float feet,
                            float *top, float *dx, float *dy, float *dz);

void rogue_platform_draw(const CraftCamera *cam, uint16_t *fb);

#endif /* ROGUE_PLATFORM_H */
