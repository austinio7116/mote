/*
 * fontdemo — a font GALLERY for Mote's proportional fonts (ABI v39).
 *
 * Every font in assets/ is baked to its own src/<name>.font.h and drawn with
 * mote->text_font over a red/blue checkerboard (so the anti-aliased edges are
 * obvious — each soft pixel blends toward whichever square is behind it). Press
 * A / B to cycle through the fonts; the sample paragraph re-wraps to the screen
 * using the selected font's own per-glyph advances, and scrolls with UP / DOWN.
 *
 * The set shows the full range: hinted TTF sans/serif/mono, a calligraphic and a
 * brush script (curvy shapes), a hand-edited glyph-sheet font, and the built-in
 * 3x5 bitmap (baked 1-bit, no AA) for contrast. The header + footer use the
 * built-in mote->text so they stay legible whatever font is selected.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "sans.font.h"
#include "serif.font.h"
#include "mono.font.h"
#include "ubuntu.font.h"
#include "bigfont.font.h"
#include "chancery.font.h"
#include "tinyfont.font.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static const MoteFont *const FONTS[] = {
    &sans, &serif, &mono, &ubuntu, &bigfont, &chancery, &tinyfont,
};
static const char *const NAMES[] = {
    "Sans", "Serif", "Mono", "Ubuntu", "bigfont (edited)", "Chancery (cursive)", "built-in 3x5 (no AA)",
};
#define NFONT ((int)(sizeof FONTS / sizeof FONTS[0]))

#define VIEW_X    4
#define VIEW_TOP  20      /* below the font-name header */
#define VIEW_BOT  118     /* above the footer */
#define WRAP_W    120
#define MAXLINES  96

static const char *PARA =
  "The quick brown fox jumps over the lazy dog. Sphinx of black quartz, judge my vow! "
  "AaBbCc DdEeFf 0123456789 ?!&@#. Press A and B to switch fonts; each one re-wraps "
  "to the screen using its own glyph widths. Coverage is packed 1/2/4-bit.";

static char  g_lines[MAXLINES][48];
static int   g_nlines, g_lh, g_cur;
static float g_scroll;

static int adv_of(const MoteFont *f, unsigned char c){
    return (c >= f->first && c < f->first + f->count) ? f->glyphs[c - f->first].adv : 0;
}
static int run_w(const MoteFont *f, const char *s, int n){ int w = 0; for (int i = 0; i < n; i++) w += adv_of(f, (unsigned char)s[i]); return w; }

/* greedy word-wrap PARA into g_lines using the SELECTED font's advances */
static void wrap(void){
    const MoteFont *f = FONTS[g_cur]; g_lh = f->line_h ? f->line_h : 8;
    g_nlines = 0; int curlen = 0, curw = 0; char cur[48]; const char *p = PARA;
    int spacew = adv_of(f, ' '); if (spacew < 1) spacew = g_lh / 3 + 1;
    while (*p) {
        const char *ws = p; while (*p && *p != ' ') p++; int wl = (int)(p - ws); while (*p == ' ') p++;
        int ww = run_w(f, ws, wl);
        if (curlen && curw + spacew + ww > WRAP_W) {
            cur[curlen] = 0;
            if (g_nlines < MAXLINES) { int i = 0; for (; cur[i]; i++) g_lines[g_nlines][i] = cur[i]; g_lines[g_nlines][i] = 0; g_nlines++; }
            curlen = 0; curw = 0;
        }
        if (curlen) { cur[curlen++] = ' '; curw += spacew; }
        for (int i = 0; i < wl && curlen < 46; i++) cur[curlen++] = ws[i];
        curw += ww;
    }
    if (curlen && g_nlines < MAXLINES) { cur[curlen] = 0; int i = 0; for (; cur[i]; i++) g_lines[g_nlines][i] = cur[i]; g_lines[g_nlines][i] = 0; g_nlines++; }
    g_scroll = 0;
}

#define CHK 8
static uint16_t checker(int x, int y){ return ((x / CHK) ^ (y / CHK)) & 1 ? MOTE_RGB565(208, 36, 36) : MOTE_RGB565(36, 56, 208); }
static void fill_checker(uint16_t *fb, int y0, int y1){
    if (y0 < 0) y0 = 0; if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
    for (int y = y0; y < y1; y++) { uint16_t *row = fb + y * MOTE_FB_W; for (int x = 0; x < MOTE_FB_W; x++) row[x] = checker(x, y); }
}

static void g_init(void){ g_cur = 0; wrap(); }
static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A)) { g_cur = (g_cur + 1) % NFONT; wrap(); }
    if (mote_just_pressed(in, MOTE_BTN_B)) { g_cur = (g_cur + NFONT - 1) % NFONT; wrap(); }
    if (mote_pressed(in, MOTE_BTN_DOWN)) g_scroll += 90.0f * dt;
    if (mote_pressed(in, MOTE_BTN_UP))   g_scroll -= 90.0f * dt;
    int viewh = VIEW_BOT - VIEW_TOP, total = g_nlines * g_lh, maxs = total - viewh; if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = 0; if (g_scroll > maxs) g_scroll = (float)maxs;
}
static void g_overlay(uint16_t *fb){
    fill_checker(fb, 0, MOTE_FB_H);
    const MoteFont *f = FONTS[g_cur]; int sc = (int)g_scroll;
    for (int i = 0; i < g_nlines; i++) {
        int ly = VIEW_TOP + i * g_lh - sc;
        if (ly >= VIEW_BOT) break;
        if (ly + g_lh <= VIEW_TOP) continue;
        mote->text_font(fb, f, g_lines[i], VIEW_X, ly, MOTE_RGB565(245, 245, 255));
    }
    /* clip the body to the scroll view, then header + footer in the built-in font */
    fill_checker(fb, 0, VIEW_TOP); fill_checker(fb, VIEW_BOT, MOTE_FB_H);
    mote->text(fb, NAMES[g_cur], 4, 2, MOTE_RGB565(255, 245, 200));
    mote->text(fb, "A/B font  UP/DOWN scroll", 4, 121, MOTE_RGB565(210, 218, 240));
    /* scrollbar */
    int viewh = VIEW_BOT - VIEW_TOP, total = g_nlines * g_lh;
    if (total > viewh) {
        int tx = 124, th = viewh * viewh / total; if (th < 10) th = 10;
        int maxs = total - viewh, ty = VIEW_TOP + (viewh - th) * sc / (maxs ? maxs : 1);
        mote->draw_rect(fb, tx, VIEW_TOP, 3, viewh, MOTE_RGB565(40, 46, 66), 1, 0, MOTE_FB_H);
        mote->draw_rect(fb, tx, ty, 3, th, MOTE_RGB565(150, 175, 225), 1, 0, MOTE_FB_H);
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("FontDemo", "mote");
