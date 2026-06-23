/*
 * Mote — launcher UI.
 */
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_font.h"
#include "mote_config.h"
#include "mote_input.h"
#include "mote_ui.h"
#include <string.h>

#ifdef MOTE_HOST
#include <stdlib.h>   /* getenv — headless test hook */
#endif

#define COL_BG     MOTE_RGB565(10, 12, 22)
#define COL_TITLE  MOTE_RGB565(120, 200, 255)
#define COL_DIM    MOTE_RGB565(120, 130, 150)
#define COL_TEXT   MOTE_RGB565(220, 228, 240)
#define COL_SEL_BG MOTE_RGB565(40, 90, 160)
#define COL_SEL_TX MOTE_RGB565(255, 255, 255)
#define COL_HINT   MOTE_RGB565(90, 100, 120)

#define LIST_Y   36
#define ROW_H    15
#define VISIBLE  5

static uint16_t s_fb[MOTE_FB_W * MOTE_FB_H];

uint16_t *mote_launcher_fb(void) { return s_fb; }

static void fill(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= MOTE_FB_H) continue;
        for (int i = x; i < x + w; i++) {
            if ((unsigned)i >= MOTE_FB_W) continue;
            s_fb[j * MOTE_FB_W + i] = c;
        }
    }
}

static void clear(uint16_t c) {
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) s_fb[i] = c;
}

static void blit_icon(const uint16_t *ic, int dx, int dy) {
    for (int y = 0; y < MOTE_ICON_H; y++) { int ry = dy + y; if ((unsigned)ry >= MOTE_FB_H) continue;
        for (int x = 0; x < MOTE_ICON_W; x++) { int rx = dx + x; if ((unsigned)rx >= MOTE_FB_W) continue;
            s_fb[ry * MOTE_FB_W + rx] = ic[y * MOTE_ICON_W + x]; } }
}
/* a stable accent colour from the name (for games with no baked icon) */
static uint16_t accent(const char *n) {
    uint32_t h = 2166136261u; for (const char *p = n; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return MOTE_RGB565(60 + (h & 127), 60 + ((h >> 8) & 127), 70 + ((h >> 16) & 127));
}
static void uppch(char *o, const char *n) { o[0] = (n[0] >= 'a' && n[0] <= 'z') ? n[0] - 32 : n[0]; o[1] = 0; }

static void draw(const MoteCatalog *cat, int sel, int top) {
    (void)top;
    mote_ui_ground(s_fb);
    mote_ui_header(s_fb, "MOTE", sel + 1, cat->count);

    if (cat->count == 0) {
        mote_font_draw(s_fb, "no games installed", (MOTE_FB_W - mote_font_width("no games installed")) / 2, 56, COL_DIM);
        mote_font_draw(s_fb, "mote push a game", (MOTE_FB_W - mote_font_width("mote push a game")) / 2, 68, COL_HINT);
        mote_ui_footer(s_fb, 0);
        return;
    }
    const char *nm = cat->e[sel].name;

    /* hero icon (left), framed + dropshadow */
    int ix = 5, iy = 24;
    fill(ix + 1, iy + 2, MOTE_ICON_W + 3, MOTE_ICON_H + 3, MOTE_RGB565(4, 6, 12));      /* shadow */
    fill(ix - 2, iy - 2, MOTE_ICON_W + 4, MOTE_ICON_H + 4, MOTE_RGB565(96, 176, 255));  /* frame */
    const uint16_t *ic = cat->e[sel].icon;
    if (ic) blit_icon(ic, ix, iy);
    else { fill(ix, iy, MOTE_ICON_W, MOTE_ICON_H, accent(nm));
        char L[2]; uppch(L, nm); mote_font_draw_2x(s_fb, L, ix + MOTE_ICON_W/2 - 5, iy + MOTE_ICON_H/2 - 7, COL_SEL_TX); }

    /* browse list (right): a window of names centred on the selection */
    int lx = 70, ly = 23, rh = 13, rows = 6;
    int start = sel - 2;
    if (start > cat->count - rows) start = cat->count - rows;
    if (start < 0) start = 0;
    for (int r = 0; r < rows && start + r < cat->count; r++) {
        int i = start + r, y = ly + r * rh;
        if (i == sel) {
            fill(lx - 2, y, MOTE_FB_W - (lx - 2) - 2, 11, MOTE_RGB565(36, 74, 138));
            fill(lx - 2, y, 2, 11, MOTE_RGB565(120, 200, 255));
            mote_font_draw(s_fb, cat->e[i].name, lx + 3, y + 2, MOTE_RGB565(255, 255, 255));
        } else {
            mote_font_draw(s_fb, cat->e[i].name, lx + 3, y + 2, MOTE_RGB565(122, 136, 164));
        }
    }
    mote_ui_footer(s_fb, "A PLAY   UP/DN BROWSE");
}

int mote_launcher_run(MoteCatalogFn rebuild) {
    int sel = 0, top = 0;
    MoteCatalog cat;   /* on the stack (SCRATCH), not BSS — OS BSS budget is ~full */
    MoteInput in;
    memset(&in, 0, sizeof in);
    /* Returning from a game, the MENU/A used to exit it may still be held — arm the
     * suppress mask so it doesn't immediately act on the launcher's first frame. */
    { MoteButtons raw0; mote_plat_buttons(&raw0); mote_input_arm(&in, &raw0); }
    uint64_t last = mote_plat_micros();

#ifdef MOTE_HOST
    const char *pick = getenv("MOTE_PICK");   /* headless: auto-select after a beat */
    int frame = 0;
#endif

    while (!mote_plat_should_quit()) {
        uint64_t now = mote_plat_micros();
        uint32_t dt_ms = (uint32_t)((now - last) / 1000);
        last = now;

        cat.count = 0;
        if (rebuild) rebuild(&cat);            /* live: reflects USB pushes */
        if (sel >= cat.count) sel = cat.count > 0 ? cat.count - 1 : 0;

        MoteButtons raw;
        mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, dt_ms);

        if (cat.count > 0) {
            if (mote_just_pressed(&in, MOTE_BTN_DOWN) || mote_just_pressed(&in, MOTE_BTN_RIGHT)) sel = (sel + 1) % cat.count;
            if (mote_just_pressed(&in, MOTE_BTN_UP)   || mote_just_pressed(&in, MOTE_BTN_LEFT))  sel = (sel - 1 + cat.count) % cat.count;
            (void)top;
            if (mote_just_pressed(&in, MOTE_BTN_A)) return sel;
        }

        /* A LAUNCH command pushed over USB (mote push --launch). */
        int pl = mote_plat_pending_launch();
        if (pl >= 0 && pl < cat.count) return pl;

#ifdef MOTE_HOST
        if (pick && ++frame > 12) return atoi(pick) % (cat.count > 0 ? cat.count : 1);
#endif

        draw(&cat, sel, top);
        mote_plat_present(s_fb);
    }
    return -1;
}
