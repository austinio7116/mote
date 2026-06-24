/*
 * ThumbyElite — ship status sheet (shared screen).
 *
 * Reachable from the station services AND the in-flight pause menu:
 * hull, mounts (quality/integrity), salvage rack, cargo, skills, money.
 */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include "craft_buttons.h"
#include <stdint.h>
#include <stdbool.h>

void status_open(void);
/* Returns true when the player closes the sheet. */
bool status_tick(const CraftRawButtons *btn, float dt);
void status_draw(uint16_t *fb);

#endif
