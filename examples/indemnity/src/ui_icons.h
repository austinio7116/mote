/*
 * ThumbyElite — tiny procedural icons (weapons etc) for richer UI.
 */
#ifndef UI_ICONS_H
#define UI_ICONS_H

#include <stdint.h>

/* 12x7 weapon glyph at (x,y), tinted by the weapon's signature colour. */
void icon_weapon(uint16_t *fb, int x, int y, int wpn_type);
/* 24x14 double-size variant for detail sheets. */
void icon_weapon_2x(uint16_t *fb, int x, int y, int wpn_type);

#endif
