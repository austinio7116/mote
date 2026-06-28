/*
 * fontdemo — anti-aliased proportional fonts (ABI v39).
 *
 * assets/bigfont.ttf is baked to src/bigfont.font.h by `mote bake` (pixel size
 * from assets/bigfont.size). The text is drawn with mote->text_font over a
 * gradient background so the anti-aliased glyph edges are visible blending into
 * the colours behind them. The paragraph is word-wrapped to the screen by the
 * game (using the font's per-glyph advances) and scrolls with UP / DOWN, with a
 * vertical scrollbar. The footer uses the built-in 3x5 mote->text() for contrast.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "bigfont.font.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define VIEW_X    4
#define VIEW_TOP  26     /* below the title */
#define VIEW_BOT  118    /* above the footer */
#define WRAP_W    118    /* wrap width in pixels */
#define MAXLINES  80

static const char *PARA =
  "Mote now bakes TrueType fonts into anti-aliased MoteFonts and draws them with "
  "mote->text_font for crisp proportional text at any size. This paragraph is "
  "word-wrapped to the screen by the game using the font's per-glyph advances, and "
  "scrolls with UP and DOWN. The smooth edges blend over the gradient behind them "
  "-- that is the antialiasing. Sphinx of black quartz, judge my vow! The quick "
  "brown fox jumps over the lazy dog. 0123456789.";

static char  g_lines[MAXLINES][48];
static int   g_nlines, g_lh;
static float g_scroll;

static int adv_of(unsigned char c){
    return (c >= bigfont.first && c < bigfont.first + bigfont.count) ? bigfont.glyphs[c - bigfont.first].adv : 0;
}
static int run_w(const char *s, int n){ int w = 0; for (int i = 0; i < n; i++) w += adv_of((unsigned char)s[i]); return w; }

/* greedy word-wrap PARA into g_lines using the baked font's advances */
static void wrap(void){
    g_nlines = 0; int curlen = 0, curw = 0; char cur[48]; const char *p = PARA;
    int spacew = adv_of(' ');
    while (*p) {
        const char *ws = p; while (*p && *p != ' ') p++; int wl = (int)(p - ws); while (*p == ' ') p++;
        int ww = run_w(ws, wl);
        if (curlen && curw + spacew + ww > WRAP_W) {        /* line full -> flush */
            cur[curlen] = 0;
            if (g_nlines < MAXLINES) { int i = 0; for (; cur[i]; i++) g_lines[g_nlines][i] = cur[i]; g_lines[g_nlines][i] = 0; g_nlines++; }
            curlen = 0; curw = 0;
        }
        if (curlen) { cur[curlen++] = ' '; curw += spacew; }
        for (int i = 0; i < wl && curlen < 46; i++) cur[curlen++] = ws[i];
        curw += ww;
    }
    if (curlen && g_nlines < MAXLINES) { cur[curlen] = 0; int i = 0; for (; cur[i]; i++) g_lines[g_nlines][i] = cur[i]; g_lines[g_nlines][i] = 0; g_nlines++; }
}

static uint16_t grad(int y){ if (y < 0) y = 0; if (y > 127) y = 127; int t = y * 255 / 127; return MOTE_RGB565(16 + t / 5, 26 + t * 2 / 5, 72 - t / 4); }
static void fill_grad(uint16_t *fb, int y0, int y1){
    if (y0 < 0) y0 = 0; if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
    for (int y = y0; y < y1; y++) { uint16_t c = grad(y); uint16_t *row = fb + y * MOTE_FB_W; for (int x = 0; x < MOTE_FB_W; x++) row[x] = c; }
}

static void g_init(void){ g_lh = bigfont.line_h; wrap(); g_scroll = 0; }
static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_pressed(in, MOTE_BTN_DOWN)) g_scroll += 90.0f * dt;
    if (mote_pressed(in, MOTE_BTN_UP))   g_scroll -= 90.0f * dt;
    int viewh = VIEW_BOT - VIEW_TOP, total = g_nlines * g_lh, maxs = total - viewh; if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = 0; if (g_scroll > maxs) g_scroll = (float)maxs;
}
static void g_overlay(uint16_t *fb){
    fill_grad(fb, 0, MOTE_FB_H);
    int sc = (int)g_scroll, viewh = VIEW_BOT - VIEW_TOP;
    for (int i = 0; i < g_nlines; i++) {
        int ly = VIEW_TOP + i * g_lh - sc;
        if (ly >= VIEW_BOT) break;
        if (ly + g_lh <= VIEW_TOP) continue;
        mote->text_font(fb, &bigfont, g_lines[i], VIEW_X, ly, MOTE_RGB565(245, 245, 255));
    }
    /* re-fill the gradient outside the scroll view to clip body bleed, then header/footer */
    fill_grad(fb, 0, VIEW_TOP); fill_grad(fb, VIEW_BOT, MOTE_FB_H);
    mote->text_font(fb, &bigfont, "Mote Fonts", 4, 2, MOTE_RGB565(255, 255, 255));
    mote->text(fb, "UP/DOWN scroll  -  AA vs built-in 3x5", 4, 121, MOTE_RGB565(210, 218, 240));
    /* scrollbar */
    int total = g_nlines * g_lh;
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
