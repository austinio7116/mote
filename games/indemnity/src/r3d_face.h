/*
 * ThumbyElite — parametric NPC portraits.
 *
 * Same philosophy as the planet impostors: appearance is a pure function
 * of a 32-bit seed, so a recurring character always shows the same face
 * and the art costs zero stored assets. Features are layered filled
 * primitives (head profile, eyes, brow, mouth, hair/headgear, markings),
 * not noise — faces need crisp symmetric features to read at 30 px.
 */
#ifndef R3D_FACE_H
#define R3D_FACE_H

#include <stdint.h>

/* Draw a portrait (panel background included) into the size*size box at
 * (bx,by). kind = NK_* from events.h biases archetype/garb; pass
 * NK_CIVILIAN for a generic face. */
void face_draw(uint16_t *fb, int bx, int by, int size, uint32_t seed,
               int kind);

/* Proposal-look switch (contact sheets only; style-1 bodies exist under
 * ELITE_STYLE_LAB). 0 = live look, the default. */
void face_set_style(int s);

#endif
