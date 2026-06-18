/*
 * Mote — text rendering (ported from ThumbyCraft craft_font).
 *
 * 3x5 bitmap glyphs in a 4x6 cell, written straight into the 128x128 RGB565
 * framebuffer. Glyph table from Pemsa (MIT). Used by the launcher and any
 * game HUD/overlay.
 */
#ifndef MOTE_FONT_H
#define MOTE_FONT_H

#include <stdint.h>

#define MOTE_FONT_CELL_W 4
#define MOTE_FONT_CELL_H 6

/* Draw text at (x,y); returns the advanced x. '\n' wraps to x and down a cell. */
int mote_font_draw(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int mote_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int mote_font_width(const char *text);
int mote_font_width_2x(const char *text);

#endif /* MOTE_FONT_H */
