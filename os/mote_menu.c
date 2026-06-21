/*
 * Mote OS — engine overlay menu. See mote_menu.h.
 */
#include "mote_menu.h"
#include "mote_platform.h"
#include "mote_input.h"
#include "mote_font.h"
#include "mote_perf.h"
#include "mote_config.h"
#include <string.h>

/* Sticky across opens so the player's choices persist within a session. */
static int s_bright = 100;
static int s_vol    = 100;

enum { M_PERF, M_BRIGHT, M_VOL, M_LOBBY, M_RESUME, M_N };
static const char *PERF_NAME[MOTE_PERF_LEVELS] = { "OFF", "FPS", "MINI", "FULL" };

#define PX 12
#define PY 14
#define PW 104
#define PH 100
#define ROW_Y (PY + 22)
#define ROW_H 15

static void fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) { if ((unsigned)j >= MOTE_FB_H) continue;
        for (int i = x; i < x + w; i++) if ((unsigned)i < MOTE_FB_W) fb[j * MOTE_FB_W + i] = c; }
}
/* darken the whole frame once (the paused game shows through behind the panel) */
static void dim(uint16_t *fb) {
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) {
        uint16_t c = fb[i];
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        fb[i] = (uint16_t)(((r / 3) << 11) | ((g / 3) << 5) | (b / 3));
    }
}
static void bar(uint16_t *fb, int x, int y, int w, int h, int pct, uint16_t fg) {
    fill(fb, x, y, w, h, MOTE_RGB565(28, 32, 44));
    fill(fb, x, y, pct * w / 100, h, fg);
}

static void draw_panel(uint16_t *fb, int sel) {
    fill(fb, PX, PY, PW, PH, MOTE_RGB565(16, 20, 32));
    fill(fb, PX, PY, PW, 1, MOTE_RGB565(80, 140, 220));
    fill(fb, PX, PY + PH - 1, PW, 1, MOTE_RGB565(80, 140, 220));
    fill(fb, PX, PY, 1, PH, MOTE_RGB565(80, 140, 220));
    fill(fb, PX + PW - 1, PY, 1, PH, MOTE_RGB565(80, 140, 220));
    mote_font_draw(fb, "ENGINE MENU", PX + 8, PY + 6, MOTE_RGB565(150, 205, 255));

    for (int i = 0; i < M_N; i++) {
        int y = ROW_Y + i * ROW_H;
        if (i == sel) fill(fb, PX + 3, y - 2, PW - 6, 12, MOTE_RGB565(40, 72, 134));
        uint16_t tc = (i == sel) ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(190, 200, 220);
        int vx = PX + 52;
        switch (i) {
            case M_PERF:   mote_font_draw(fb, "PERF",   PX + 8, y, tc);
                           mote_font_draw(fb, PERF_NAME[mote_perf_level()], vx, y, MOTE_RGB565(150, 230, 150)); break;
            case M_BRIGHT: mote_font_draw(fb, "BRIGHT", PX + 8, y, tc);
                           bar(fb, vx, y, 44, 7, s_bright, MOTE_RGB565(240, 210, 90)); break;
            case M_VOL:    mote_font_draw(fb, "VOLUME", PX + 8, y, tc);
                           bar(fb, vx, y, 44, 7, s_vol, MOTE_RGB565(120, 190, 255)); break;
            case M_LOBBY:  mote_font_draw(fb, "RETURN TO LOBBY", PX + 8, y, tc); break;
            case M_RESUME: mote_font_draw(fb, "RESUME", PX + 8, y, tc); break;
        }
    }
    mote_font_draw(fb, "L/R adjust  B close", PX + 6, PY + PH - 9, MOTE_RGB565(110, 120, 142));
}

int mote_engine_menu(uint16_t *fb) {
    MoteInput in;
    memset(&in, 0, sizeof in);
    int sel = 0, armed = 0;            /* armed once MENU is released (it's held on open) */
    uint64_t last = mote_plat_micros();

    mote_plat_set_brightness(s_bright);
    mote_plat_set_volume(s_vol);
    dim(fb);                           /* darken the frozen frame once */

    while (!mote_plat_should_quit()) {
        uint64_t now = mote_plat_micros();
        uint32_t dt_ms = (uint32_t)((now - last) / 1000); last = now;
        MoteButtons raw; mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, dt_ms);

        if (!mote_pressed(&in, MOTE_BTN_MENU)) armed = 1;
        if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % M_N;
        if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel - 1 + M_N) % M_N;

        int d = mote_just_pressed(&in, MOTE_BTN_RIGHT) ? 1 : (mote_just_pressed(&in, MOTE_BTN_LEFT) ? -1 : 0);
        if (d) {
            if (sel == M_PERF)        mote_perf_set_level(mote_perf_level() + d);
            else if (sel == M_BRIGHT) { s_bright += d * 10; if (s_bright < 10) s_bright = 10; if (s_bright > 100) s_bright = 100;
                                        mote_plat_set_brightness(s_bright); }
            else if (sel == M_VOL)    { s_vol += d * 10; if (s_vol < 0) s_vol = 0; if (s_vol > 100) s_vol = 100;
                                        mote_plat_set_volume(s_vol); }
        }
        int ret = -1;
        if (mote_just_pressed(&in, MOTE_BTN_A)) {
            if (sel == M_LOBBY)  ret = 1;
            else if (sel == M_RESUME) ret = 0;
            else if (sel == M_PERF)   mote_perf_set_level((mote_perf_level() + 1) % MOTE_PERF_LEVELS);
        }
        if (armed && (mote_just_pressed(&in, MOTE_BTN_B) || mote_just_pressed(&in, MOTE_BTN_MENU)))
            ret = 0;

        if (ret >= 0) {
            /* Drain A/B/MENU before handing control back, so the game or launcher
             * doesn't see a phantom just-pressed from the button we exited on. */
            for (;;) { MoteButtons r; mote_plat_buttons(&r);
                if ((!r.a && !r.b && !r.menu) || mote_plat_should_quit()) break;
                mote_plat_present(fb); }
            return ret;
        }

        draw_panel(fb, sel);
        mote_plat_present(fb);
    }
    return 0;
}
