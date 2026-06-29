#ifndef ROGUE_LOOT_H
#define ROGUE_LOOT_H
/*
 * ThumbyRogue ground loot + chests. Enemies drop items here; chests in
 * rooms hold loot. Gold and potions are picked up by walking over them;
 * weapons are equipped via MENU (the old weapon drops so you can swap back).
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "craft_render.h"
#include "rogue_items.h"
#include "rogue_player.h"

void rogue_loot_clear(void);
void rogue_loot_drop(const RogueItem *it, Vec3 pos);

/* Scatter depth-scaled chests across rooms (never the up-stairs room). */
void rogue_loot_place_chests(const int16_t *room_cx, const int16_t *room_cz,
                             int n_rooms, int up_x, int up_z,
                             int floor_y, int depth, uint32_t seed);

/* Walk-over pickups (gold/potions) applied to the player. */
void rogue_loot_update(RoguePlayer *p, float dt);

/* Nearest equippable weapon within interact range; for the HUD prompt. */
bool rogue_loot_weapon_near(float x, float z, RogueItem *out, int *out_index);
/* Remove + return the ground weapon at index (after equipping it). */
bool rogue_loot_take(int index, RogueItem *out);

/* Nearest unopened chest within interact range (XZ + matching height, so
 * pedestal chests require a jump up to open). */
bool rogue_loot_chest_near(float x, float y, float z, int *out_index);
void rogue_loot_open_chest(int index, int depth, uint32_t seed);
/* Place a single chest at an exact spot (e.g. a bonus chest on a lava island). */
void rogue_loot_add_chest_at(float x, float y, float z);

void rogue_loot_draw(const CraftCamera *cam, uint16_t *fb);

/* --- mid-level suspend snapshot --------------------------------------- *
 * Ground drops aren't reproducible from the seed (they come from kills /
 * opened chests), so they're saved in full. Chest *positions* regenerate
 * deterministically, so only their opened-flags need restoring. */
#define ROGUE_MAX_GROUND 18
typedef struct { RogueItem it; Vec3 pos; } RogueGroundSave;
int  rogue_loot_export_ground(RogueGroundSave *out, int max);   /* live drops */
void rogue_loot_import_ground(const RogueGroundSave *in, int n);
uint32_t rogue_loot_chest_mask(void);          /* bit i set = chest i opened */
void rogue_loot_apply_chest_mask(uint32_t mask);

#endif /* ROGUE_LOOT_H */
