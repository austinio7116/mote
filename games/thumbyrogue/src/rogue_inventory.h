#ifndef ROGUE_INVENTORY_H
#define ROGUE_INVENTORY_H
/*
 * ThumbyRogue backpack + paperdoll inventory screen. Loot flows into the
 * backpack; MENU opens this screen to compare, equip (swap), unequip, and
 * salvage gear. Operates directly on the player's equip[] + gold.
 */
#include <stdint.h>
#include <stdbool.h>
#include "rogue_player.h"
#include "craft_buttons.h"

#define ROGUE_BAG_N 21   /* 7 columns x 3 rows in the backpack grid */

void rogue_inventory_clear(void);
bool rogue_inventory_add(const RogueItem *it);   /* false if backpack full */
bool rogue_inventory_full(void);
int  rogue_inventory_count(void);

/* Save/load: copy the backpack out / in. */
int  rogue_inventory_export(RogueItem *out, int max);
void rogue_inventory_import(const RogueItem *in, int n);

bool rogue_inventory_is_open(void);
bool rogue_inventory_detail_open(void);   /* item-detail / gem-pick / confirm sub-screen */
void rogue_inventory_open(void);
void rogue_inventory_close(void);

/* Process one frame of inventory input (edges vs prev). */
void rogue_inventory_input(RoguePlayer *p, const CraftRawButtons *btn,
                           const CraftRawButtons *prev);
void rogue_inventory_draw(uint16_t *fb, const RoguePlayer *p);

/* Tiny per-item-type glyph into a ~12x9 box at (ox,oy); tint = rarity/item
 * colour. Shared by the backpack grid and the merchant shop. */
void rogue_item_draw_icon(uint16_t *fb, int ox, int oy, const RogueItem *it, uint16_t tint);

#endif /* ROGUE_INVENTORY_H */
