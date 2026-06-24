/*
 * ThumbyCraft — text rendering.
 *
 * 3×5 bitmap glyphs in a 4×6 cell, written directly into the 128×128
 * RGB565 framebuffer. The glyph table was originally transcribed by
 * the ThumbyDOOM/ThumbyNES projects from Pemsa (MIT-licensed).
 */
#ifndef CRAFT_FONT_H
#define CRAFT_FONT_H

#include "cue_types.h"

#define CRAFT_FONT_CELL_W 4
#define CRAFT_FONT_CELL_H 6

int craft_font_draw(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int craft_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color);
/* Scaled title text (s = pixel size) with a vertical gradient + 1px outline. */
int craft_font_draw_title(uint16_t *fb, const char *text, int x, int y, int s,
                          uint16_t top, uint16_t bot, uint16_t outline);
int craft_font_width(const char *text);
int craft_font_width_2x(const char *text);
/* Fractional scale (num/den), e.g. 3/2 = 1.5x — a size between 1x and 2x. */
int craft_font_draw_frac(uint16_t *fb, const char *text, int x, int y,
                         int num, int den, uint16_t color);
/* Anti-aliased fractional scale: blends coverage onto the existing fb pixels. */
int craft_font_draw_frac_aa(uint16_t *fb, const char *text, int x, int y,
                            int num, int den, uint16_t color);
int craft_font_width_frac(const char *text, int num, int den);

/* Set the destination dimensions glyph writes are clamped/strided to. Used by
 * the HUD when rendering into a smaller upscale overlay. No-op (and the writer
 * stays a compile-time constant) unless CRAFT_HUD_SCALE>1. */
void craft_font_set_target(int w, int h);

#endif
