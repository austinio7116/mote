/* Readable menu/panel text for Indemnity Run — see elite_ui.h. */
#include "elite_ui.h"
#include "craft_font.h"
#include "elite_engine.h"   /* extern const MoteApi *g_em (engine jump table) */
#include "mote_api.h"
#include "mote_build.h"     /* mote_ftext / mote_ftextc / mote_fontw */
#include <string.h>
#include <stdio.h>

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

int  eui_fit(int height, int n, int *pitch){
    eui_bind();
    int minp = AA ? 11 : 8;                 /* tightest legible row pitch (glyph height) */
    int rows = height / minp;
    if (rows < 1) rows = 1;
    if (rows > n) rows = n;
    int p = rows > 0 ? height / rows : minp; /* spread the rows to fill the window */
    if (p < minp) p = minp;
    if (pitch) *pitch = p;
    return rows;
}
int  eui_textw(const char *s){ eui_bind(); return AA ? mote_fontw(g_body, s) : craft_font_width(s); }

void eui_text(uint16_t *fb, const char *s, int x, int y, uint16_t c){
    eui_bind();
    if (AA) mote_ftext(g_em, fb, g_body, s, x, y, c);
    else    craft_font_draw(fb, s, x, y, c);
}
void eui_textc(uint16_t *fb, const char *s, int cx, int y, uint16_t c){ eui_text(fb, s, cx - eui_textw(s)/2, y, c); }
void eui_textr(uint16_t *fb, const char *s, int xr, int y, uint16_t c){ eui_text(fb, s, xr - eui_textw(s),   y, c); }
void eui_textclip(uint16_t *fb, const char *s, int x, int xmax, int y, uint16_t c){
    char t[48]; snprintf(t, sizeof t, "%s", s);
    for (int n = (int)strlen(t); n > 0 && x + eui_textw(t) > xmax; n--) t[n - 1] = 0;
    eui_text(fb, t, x, y, c);
}
void eui_big(uint16_t *fb, const char *s, int cx, int y, uint16_t c){
    eui_bind();
    if (AA && g_big) mote_ftextc(g_em, fb, g_big, cx, y, c, s);
    else eui_textc(fb, s, cx, y, c);
}
int  eui_bigw(const char *s){ eui_bind(); return (AA && g_big) ? mote_fontw(g_big, s) : eui_textw(s); }
void eui_textbig(uint16_t *fb, const char *s, int x, int y, uint16_t c){
    eui_bind();
    if (AA && g_big) mote_ftext(g_em, fb, g_big, s, x, y, c);
    else eui_text(fb, s, x, y, c);
}
void eui_textbigr(uint16_t *fb, const char *s, int xr, int y, uint16_t c){ eui_textbig(fb, s, xr - eui_bigw(s), y, c); }

int eui_list(uint16_t *fb, const char *const *items, int n, int cursor, int scroll,
             int x, int y0, int y1, uint16_t sel, uint16_t dim){
    eui_bind();
    int lh;
    int rows = eui_fit(y1 - y0, n, &lh);
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
    eui_scrollbar(fb, 125, y0, y1 - y0, n, rows, scroll, sel, dim);
    return scroll;
}

/* Shared word-wrap core: draws lines of `text` in [x0,x1) from y, stopping
   before ymax. Writes the final y to *out_y and returns a pointer to the first
   character NOT drawn (== end of string if everything fit). */
static const char *wrap_core(uint16_t *fb, const char *text, int x0, int x1,
                             int y, int ymax, uint16_t col, int *out_y){
    eui_bind();
    int maxw = x1 - x0, lh = eui_lineh();
    char line[64]; line[0] = 0;
    const char *p = text, *line_start = text;
    while (*p){
        while (*p == ' ') p++;                     /* eat run of spaces */
        const char *w0 = p;
        while (*p && *p != ' ' && *p != '\n') p++; /* one word [w0,p) */
        int wl = (int)(p - w0);
        if (wl > 0){
            char cand[80];
            int ll = (int)strlen(line);
            if (wl > (int)sizeof line - 1) wl = sizeof line - 1;
            if (ll == 0){ snprintf(cand, sizeof cand, "%.*s", wl, w0); line_start = w0; }
            else          snprintf(cand, sizeof cand, "%s %.*s", line, wl, w0);
            if (ll > 0 && eui_textw(cand) > maxw){ /* word overflows: flush, start anew */
                if (y + lh > ymax){ *out_y = y; return line_start; }
                if (fb) eui_text(fb, line, x0, y, col); y += lh;   /* fb==NULL: measure only */
                snprintf(line, sizeof line, "%.*s", wl, w0);
                line_start = w0;
            } else {
                snprintf(line, sizeof line, "%s", cand);
            }
        }
        if (*p == '\n'){                            /* hard break */
            if (line[0]){
                if (y + lh > ymax){ *out_y = y; return line_start; }
                if (fb) eui_text(fb, line, x0, y, col); y += lh; line[0] = 0;
            }
            p++; line_start = p;
        }
    }
    if (line[0]){
        if (y + lh > ymax){ *out_y = y; return line_start; }
        if (fb) eui_text(fb, line, x0, y, col); y += lh;
    }
    *out_y = y;
    return p;
}

int eui_wrap(uint16_t *fb, const char *text, int x0, int x1, int y, int ymax, uint16_t col){
    int oy = y; wrap_core(fb, text, x0, x1, y, ymax, col, &oy); return oy;
}
const char *eui_wrapt(uint16_t *fb, const char *text, int x0, int x1, int y, int ymax, uint16_t col){
    int oy = y; return wrap_core(fb, text, x0, x1, y, ymax, col, &oy);
}

int eui_wrap_scroll(uint16_t *fb, const char *text, int x0, int x1,
                    int y0, int y1, int scroll, uint16_t col){
    eui_bind();
    int maxw = x1 - x0, lh = eui_lineh();
    int vis = (y1 - y0) / lh; if (vis < 1) vis = 1;
    char line[64]; line[0] = 0;
    const char *p = text;
    int idx = 0;
#define EUI_EMIT() do {                                                     \
        if (fb && idx >= scroll && idx < scroll + vis)                      \
            eui_text(fb, line, x0, y0 + (idx - scroll) * lh, col);          \
        idx++; line[0] = 0;                                                 \
    } while (0)
    while (*p){
        while (*p == ' ') p++;
        const char *w0 = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        int wl = (int)(p - w0);
        if (wl > 0){
            char cand[80];
            int ll = (int)strlen(line);
            if (wl > (int)sizeof line - 1) wl = sizeof line - 1;
            if (ll == 0) snprintf(cand, sizeof cand, "%.*s", wl, w0);
            else         snprintf(cand, sizeof cand, "%s %.*s", line, wl, w0);
            if (ll > 0 && eui_textw(cand) > maxw){ EUI_EMIT();
                snprintf(line, sizeof line, "%.*s", wl, w0);
            } else snprintf(line, sizeof line, "%s", cand);
        }
        if (*p == '\n'){ if (line[0]) EUI_EMIT(); p++; }
    }
    if (line[0]) EUI_EMIT();
#undef EUI_EMIT
    return idx;
}

void eui_scrollbar(uint16_t *fb, int bx, int y0, int rows_px, int n, int rows, int scroll,
                   uint16_t sel, uint16_t dim){
    if (n <= rows || !g_em || !g_em->draw_rect) return;
    g_em->draw_rect(fb, bx, y0, 2, rows_px, dim, 1, 0, 128);               /* track */
    int th = rows_px*rows/n; if (th < 4) th = 4;
    int ty = y0 + (rows_px - th)*scroll/(n - rows);
    g_em->draw_rect(fb, bx, ty, 2, th, sel, 1, 0, 128);                    /* thumb */
}
