#ifndef ROGUE_SHOP_H
#define ROGUE_SHOP_H
/*
 * ThumbyRogue shop — one merchant stall per level (counter + shopkeeper +
 * shelf wall, built by the generator). Walk up to the counter to trade:
 * buy rolled gear with gold, GAMBLE for a random item, or REROLL / UPGRADE
 * the equipped weapon. Bought items go to the backpack.
 */
#include <stdint.h>
#include <stdbool.h>
#include "craft_types.h"
#include "rogue_player.h"
#include "rogue_gen.h"
#include "craft_buttons.h"

void rogue_shop_place(const RogueLevelInfo *lv, int depth, uint32_t seed);
bool rogue_shop_pad_near(float x, float y, float z);
int  rogue_shop_take_stock(RogueItem *out);   /* spill wares on merchant death */

bool rogue_shop_is_open(void);
void rogue_shop_open(void);
void rogue_shop_close(void);
void rogue_shop_input(RoguePlayer *p, const CraftRawButtons *btn,
                      const CraftRawButtons *prev);
void rogue_shop_draw(uint16_t *fb, const RoguePlayer *p);

#endif /* ROGUE_SHOP_H */
