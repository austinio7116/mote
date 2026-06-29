#ifndef ROGUE_RENDER_H
#define ROGUE_RENDER_H
/*
 * ThumbyRogue cuboid entity renderer.
 *
 * Decoupled re-implementation of ThumbyCraft's mob/torch cuboid render
 * (per-pixel ray-vs-AABB into the screen bbox, z-tested against the global
 * craft_zbuf the world raycaster fills). Draws the player, enemies, chests,
 * pickups and stairs as little multi-cuboid voxel models.
 *
 * Parts are defined in MODEL-LOCAL space: feet at y=0, +Z is "forward",
 * centred on x=z=0. The model's world feet-position + a single yaw rotate it
 * into the scene — same convention as craft_mobs.c.
 */
#include <stdint.h>
#include "craft_types.h"
#include "craft_render.h"

typedef struct {
    float cx, cy, cz;     /* part centre, model-local */
    float hx, hy, hz;     /* half-extents */
    uint16_t color;       /* RGB565, baked per-part (no atlas) */
} RogueCuboid;

/* Render one cuboid model. `radius`/`height` bound the model for the screen
 * bbox cull (radius = max |x|,|z|; height = top y). `flash` 0..1 washes the
 * model red (hit feedback); `tint_q8` scales brightness (256 = unchanged,
 * used for darkness/lighting). */
void rogue_render_model(const CraftCamera *cam, uint16_t *fb,
                        Vec3 pos, float yaw,
                        const RogueCuboid *parts, int n_parts,
                        float radius, float height,
                        float flash, int tint_q8);

#endif /* ROGUE_RENDER_H */
