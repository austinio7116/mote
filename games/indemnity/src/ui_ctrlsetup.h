/*
 * ThumbyElite — in-game CONTROLLER SETUP screen (PC HOTAS binding).
 *
 * A generic press-to-bind list over the plat_ctrl_* interface; the shell
 * owns the device. Reached from SETTINGS on shells that have a controller.
 */
#ifndef UI_CTRLSETUP_H
#define UI_CTRLSETUP_H

#include <stdint.h>
#include <stdbool.h>
#include "craft_buttons.h"

void ctrlsetup_open(void);
bool ctrlsetup_tick(const CraftRawButtons *btn, float dt);  /* true = close */
void ctrlsetup_draw(uint16_t *fb);

#endif
