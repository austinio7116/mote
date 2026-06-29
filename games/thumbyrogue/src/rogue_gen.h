#ifndef ROGUE_GEN_H
#define ROGUE_GEN_H
/*
 * ThumbyRogue dungeon generator. Carves a BSP rooms+corridors dungeon into
 * the static 64^3 craft_world buffer (origin 0,0). BSP recursion connects
 * every sibling region, so the room graph is fully connected — the down-
 * stairs are always reachable from spawn (verified by a flood-fill).
 */
#include <stdint.h>
#include "craft_types.h"

#define ROGUE_MAX_LEVEL_ROOMS 48
#define ROGUE_MAX_PROPS       24

/* Furniture props drawn per-frame as cuboid models (like torches), kept
 * out of the world buffer because they need sub-cube geometry. */
typedef enum {
    PROP_TABLE   = 0,   /* wooden table — top slab on four legs */
    PROP_BRAZIER = 1,   /* iron bowl + flame on a tripod (seeds a light cell) */
} RoguePropKind;

typedef struct {
    Vec3 spawn;            /* hero feet, on the up-stairs */
    int  up_x, up_z;       /* up-stairs entry cell (XZ) */
    int  down_x, down_z;   /* down-stairs entry cell (XZ) */
    int8_t up_dx, up_dz;   /* direction the up staircase rises */
    int8_t down_dx, down_dz; /* direction the down trench descends */
    int8_t down_px, down_pz; /* perpendicular toward the 2nd width column */
    int8_t down_wide;        /* 1 = trench is two cells wide */
    uint8_t has_shop;        /* a merchant stall was built this floor */
    int16_t shop_x, shop_z;  /* counter centre cell */
    int8_t shop_dx, shop_dz; /* counter front: direction toward the customer */
    int  floor_y;          /* walkable surface Y */
    int  n_rooms;
    int16_t room_cx[ROGUE_MAX_LEVEL_ROOMS];
    int16_t room_cz[ROGUE_MAX_LEVEL_ROOMS];
    int  n_chasm;                  /* lava-chasm rooms (bridge + platform) */
    int16_t chasm_x[3], chasm_z[3];
    int16_t island_x[3], island_z[3];  /* bonus-chest island in each chasm */
    int  n_torch;                  /* wall/floor torch light positions */
    int16_t torch_x[16], torch_z[16];
    int  n_prop;                   /* furniture props (table/brazier models) */
    int16_t prop_x[ROGUE_MAX_PROPS], prop_z[ROGUE_MAX_PROPS];
    uint8_t prop_kind[ROGUE_MAX_PROPS];
} RogueLevelInfo;

void rogue_gen_dungeon(uint32_t seed, int depth, RogueLevelInfo *out);

#endif /* ROGUE_GEN_H */
