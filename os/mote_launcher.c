/*
 * Mote — launcher UI.
 */
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_font.h"
#include "mote_config.h"
#include "mote_input.h"
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

static void draw(const MoteCatalog *cat, int sel, int top) {
    clear(COL_BG);

    /* Title. */
    const char *title = "MOTE";
    mote_font_draw_2x(s_fb, title, (MOTE_FB_W - mote_font_width_2x(title)) / 2, 6, COL_TITLE);
    const char *sub = "select a game";
    mote_font_draw(s_fb, sub, (MOTE_FB_W - mote_font_width(sub)) / 2, 24, COL_DIM);

    if (cat->count == 0) {
        const char *none = "no games installed";
        mote_font_draw(s_fb, none, (MOTE_FB_W - mote_font_width(none)) / 2, 60, COL_DIM);
    }

    for (int row = 0; row < VISIBLE; row++) {
        int idx = top + row;
        if (idx >= cat->count) break;
        int y = LIST_Y + row * ROW_H;
        bool is_sel = (idx == sel);
        if (is_sel) fill(8, y - 2, MOTE_FB_W - 16, ROW_H - 2, COL_SEL_BG);
        mote_font_draw(s_fb, cat->e[idx].name, 16, y + 2, is_sel ? COL_SEL_TX : COL_TEXT);
    }

    /* Scroll arrows. */
    if (top > 0)                       mote_font_draw(s_fb, "^", MOTE_FB_W - 12, LIST_Y, COL_DIM);
    if (top + VISIBLE < cat->count)    mote_font_draw(s_fb, "v", MOTE_FB_W - 12, LIST_Y + (VISIBLE - 1) * ROW_H, COL_DIM);

    /* Footer hint. */
    const char *hint = "A play";
    mote_font_draw(s_fb, hint, (MOTE_FB_W - mote_font_width(hint)) / 2, MOTE_FB_H - 10, COL_HINT);
}

int mote_launcher_run(MoteCatalogFn rebuild) {
    int sel = 0, top = 0;
    MoteCatalog cat;
    MoteInput in;
    memset(&in, 0, sizeof in);
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
            if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % cat.count;
            if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel - 1 + cat.count) % cat.count;
            if (sel < top)               top = sel;
            if (sel >= top + VISIBLE)    top = sel - VISIBLE + 1;
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
