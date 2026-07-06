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
/* The standard Mote palette (RGB565), shared so every screen matches the chrome. */
#define MOTE_UI_BAR     0x10A5   /* header/footer bar          (18,22,40)   */
#define MOTE_UI_ACCENT  0x659F   /* accent rule / arrows       (96,176,255) */
#define MOTE_UI_GOLD    0xFE6B   /* title gold                 (255,206,92) */
#define MOTE_UI_TEXT    0xE73E   /* body text                  (224,230,244)*/
#define MOTE_UI_DIM     0x7C34   /* dim / hint text            (120,134,162)*/

/* Anti-aliased proportional UI fonts (baked from Ubuntu): MEDIUM replaces the old
 * 1x bitmap for body/list/detail text, LARGE replaces 2x for titles. Top-anchored
 * at (x,y) like the bitmap font; coverage tables are const (flash, zero RAM). */
void mote_ui_text  (uint16_t *fb, const char *s, int x, int y, uint16_t color);  /* medium (1.5x) */
void mote_ui_title (uint16_t *fb, const char *s, int x, int y, uint16_t color);  /* large  (2x)   */
void mote_ui_read  (uint16_t *fb, const char *s, int x, int y, uint16_t color);  /* reading(1.66x) */
int  mote_ui_text_w (const char *s);   /* medium pixel width */
int  mote_ui_title_w(const char *s);   /* large pixel width  */
int  mote_ui_read_w (const char *s);   /* reading pixel width */
int  mote_ui_text_h (void);            /* medium line height */
int  mote_ui_title_h(void);            /* large line height  */
int  mote_ui_read_h (void);            /* reading line height */

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
