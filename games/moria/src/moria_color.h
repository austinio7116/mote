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
