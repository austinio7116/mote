#ifndef ELITE_UI_H
#define ELITE_UI_H
#include <stdint.h>

/* Readable UI text for Indemnity Run's MENUS/PANELS — draws with the engine's
 * anti-aliased Audiowide font when the firmware exposes it (ABI v47), else falls
 * back to the compact craft bitmap font. (The real-time flight/combat HUD keeps
 * using craft_font directly — it must stay dense.) Lazily binds to the engine via
 * the global g_em, so no init wiring is needed. */
int  eui_ready(void);                 /* 1 if the Audiowide font is active */
int  eui_lineh(void);                 /* body line height (px) — a good row pitch base */
/* Pick the number of rows that best fills a `height`-px window for `n` items: as many
 * readable rows as fit (down to the tightest legible pitch), spread to fill the height.
 * Writes the row pitch to *pitch. Use for scrolling lists so they fill the space. */
int  eui_fit(int height, int n, int *pitch);
int  eui_textw(const char *s);        /* body-text pixel width */
void eui_text (uint16_t *fb, const char *s, int x,  int y, uint16_t col);   /* left at (x,y) */
void eui_textc(uint16_t *fb, const char *s, int cx, int y, uint16_t col);   /* centered on cx */
void eui_textr(uint16_t *fb, const char *s, int xr, int y, uint16_t col);   /* right edge at xr */
void eui_big  (uint16_t *fb, const char *s, int cx, int y, uint16_t col);   /* large centered header */

/* A scrollable vertical menu list drawn between y0 and y1 at left margin x. Draws a
 * caret on `cursor`, keeps it visible, and draws a scrollbar on the right when the
 * list is taller than the window. Returns the (possibly adjusted) scroll offset —
 * store it back so the caller's scroll state tracks the view. */
int  eui_list (uint16_t *fb, const char *const *items, int n, int cursor, int scroll,
               int x, int y0, int y1, uint16_t sel, uint16_t dim);

/* Draw just the scrollbar (for a hand-rolled scrolling list that needs per-row extras).
 * `rows` = visible rows, `n` = total items. No-op when everything fits. */
void eui_scrollbar(uint16_t *fb, int bx, int y0, int rows_px, int n, int rows, int scroll,
                   uint16_t sel, uint16_t dim);

#endif /* ELITE_UI_H */
