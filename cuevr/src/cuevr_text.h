/* CueVR — AA proportional text into an arbitrarily sized RGB565 target.
 * See cuevr_text.c for why this is not mote_font_draw_aa. */
#ifndef CUEVR_TEXT_H
#define CUEVR_TEXT_H
#include <stdint.h>
#include <stddef.h>
#include "mote_2d.h"
int cuevr_text_draw(uint16_t *fb, int tw, int th, const MoteFont *f,
                    const char *s, int x, int y, uint16_t colour);
int cuevr_text_width(const MoteFont *f, const char *s);
#endif
