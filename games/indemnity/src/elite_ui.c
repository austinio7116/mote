/* Readable menu/panel text for Indemnity Run — see elite_ui.h. */
#include "elite_ui.h"
#include "craft_font.h"
#include "elite_engine.h"   /* extern const MoteApi *g_em (engine jump table) */
#include "mote_api.h"
#include "mote_build.h"     /* mote_ftext / mote_ftextc / mote_fontw */

static const MoteFont *g_body;   /* MED ~11px (1.5x) — labels/values/list rows */
static const MoteFont *g_big;    /* LARGE ~15px (2x)  — panel headers          */
static int g_bound;

static void eui_bind(void){
    if (g_bound) return;
    g_bound = 1;
    if (g_em && g_em->abi_version >= 47 && g_em->ui_font){
        g_body = g_em->ui_font(MOTE_FONT_MED);
        g_big  = g_em->ui_font(MOTE_FONT_LARGE);
    }
}
#define AA (g_em && g_body)

int  eui_ready(void){ eui_bind(); return AA ? 1 : 0; }
int  eui_lineh(void){ eui_bind(); return AA ? 12 : 9; }
int  eui_textw(const char *s){ eui_bind(); return AA ? mote_fontw(g_body, s) : craft_font_width(s); }

void eui_text(uint16_t *fb, const char *s, int x, int y, uint16_t c){
    eui_bind();
    if (AA) mote_ftext(g_em, fb, g_body, s, x, y, c);
    else    craft_font_draw(fb, s, x, y, c);
}
void eui_textc(uint16_t *fb, const char *s, int cx, int y, uint16_t c){ eui_text(fb, s, cx - eui_textw(s)/2, y, c); }
void eui_textr(uint16_t *fb, const char *s, int xr, int y, uint16_t c){ eui_text(fb, s, xr - eui_textw(s),   y, c); }
void eui_big(uint16_t *fb, const char *s, int cx, int y, uint16_t c){
    eui_bind();
    if (AA && g_big) mote_ftextc(g_em, fb, g_big, cx, y, c, s);
    else eui_textc(fb, s, cx, y, c);
}

int eui_list(uint16_t *fb, const char *const *items, int n, int cursor, int scroll,
             int x, int y0, int y1, uint16_t sel, uint16_t dim){
    eui_bind();
    int lh = eui_lineh() + 2;
    int rows = (y1 - y0) / lh; if (rows < 1) rows = 1;
    if (cursor < scroll)          scroll = cursor;
    if (cursor >= scroll + rows)  scroll = cursor - rows + 1;
    if (n > rows && scroll > n - rows) scroll = n - rows;
    if (scroll < 0) scroll = 0;
    for (int r = 0; r < rows && scroll + r < n; r++){
        int i = scroll + r, y = y0 + r*lh;
        uint16_t c = (i == cursor) ? sel : dim;
        if (i == cursor) eui_text(fb, ">", x - 9, y, c);
        eui_text(fb, items[i], x, y, c);
    }
    eui_scrollbar(fb, 125, y0, rows*lh - 2, n, rows, scroll, sel, dim);
    return scroll;
}

void eui_scrollbar(uint16_t *fb, int bx, int y0, int rows_px, int n, int rows, int scroll,
                   uint16_t sel, uint16_t dim){
    if (n <= rows || !g_em || !g_em->draw_rect) return;
    g_em->draw_rect(fb, bx, y0, 2, rows_px, dim, 1, 0, 128);               /* track */
    int th = rows_px*rows/n; if (th < 4) th = 4;
    int ty = y0 + (rows_px - th)*scroll/(n - rows);
    g_em->draw_rect(fb, bx, ty, 2, th, sel, 1, 0, 128);                    /* thumb */
}
