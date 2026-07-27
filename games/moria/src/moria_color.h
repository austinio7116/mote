/* games/moria/src/moria_color.h: 16-colour palette (Umoria 5.6 is monochrome)

   Copyright (C) 2026 austinio7116

   This file is part of the Mote port of Umoria to the Thumby Color, and is
   distributed as part of that combined work.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>. */

/* moria_color.h : the 16-colour palette indices + the shared draw-colour used
   by the Mote port to colourise Umoria's monochrome display.

   Included by both curses.h (renderer/terminal side) and externs.h (game-core
   side) so loc_symbol() / the spell functions and the renderer agree on the
   index values.  Renderer-only: no game logic depends on colour. */

#ifndef MORIA_COLOR_H
#define MORIA_COLOR_H

/* Angband-order 16-colour palette indices. */
#define MC_DEFAULT 0
#define MC_WHITE   1
#define MC_SLATE   2
#define MC_ORANGE  3
#define MC_RED     4
#define MC_GREEN   5
#define MC_BLUE    6
#define MC_UMBER   7
#define MC_LDARK   8
#define MC_LWHITE  9
#define MC_VIOLET  10
#define MC_YELLOW  11
#define MC_LRED    12
#define MC_LGREEN  13
#define MC_LBLUE   14
#define MC_LUMBER  15

/* Colour of the next glyph the core draws through print()/mvaddch.  Set it
   before a coloured draw; string writes reset it to MC_DEFAULT. */
extern unsigned char mote_draw_color;

unsigned char moria_monster_color(int cchar);  /* misc1.c */
unsigned char moria_object_color(int tval);    /* misc1.c */

#endif /* MORIA_COLOR_H */
