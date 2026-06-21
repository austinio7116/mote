/*
 * Mote OS — shared styled-UI kit (console look: dark ground, gold title, accent
 * header/footer rules, a highlighted selection bar). Used by the launcher, the
 * engine menu, and — via mote->menu() — by games building their own menus, so
 * every menu in the system looks the same.
 */
#ifndef MOTE_UI_H
#define MOTE_UI_H

#include <stdint.h>

#define MOTE_UI_BG      0x10A4   /* dark navy ground */

/* Darken the current frame (for modal overlays over a live game). */
void mote_ui_dim(uint16_t *fb);
/* Fill the whole frame with the menu ground. */
void mote_ui_ground(uint16_t *fb);
/* Title bar (gold title, accent underline). `count`<0 hides the right counter. */
void mote_ui_header(uint16_t *fb, const char *title, int idx, int count);
/* Footer bar (accent overline + control hint). */
void mote_ui_footer(uint16_t *fb, const char *hint);
/* A scrollable highlighted list between y0 and the footer. Returns the (possibly
 * adjusted) scroll `top` that keeps `sel` visible. */
int  mote_ui_list(uint16_t *fb, const char *const *items, int n, int sel, int top, int y0);

#endif /* MOTE_UI_H */
