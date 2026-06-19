/*
 * Mote — performance overlay.
 */
#include "mote_perf.h"
#include "mote_config.h"
#include "mote_font.h"
#include <stdio.h>

#define HIST 100

typedef struct { uint16_t update, raster, flush, c1, frame; } Sample;

static Sample s_hist[HIST];
static int    s_head;
static int    s_on;

void mote_perf_toggle(void)  { s_on = !s_on; }
int  mote_perf_enabled(void) { return s_on; }

static inline uint16_t c16(uint32_t v) { return v > 65535u ? 65535u : (uint16_t)v; }

void mote_perf_record(uint32_t upd, uint32_t c0, uint32_t c1,
                      uint32_t flush, uint32_t frame) {
    Sample *s = &s_hist[s_head];
    s->update = c16(upd);
    s->raster = c16(c0 > c1 ? c0 : c1);   /* wall-clock raster = slower core */
    s->flush  = c16(flush);
    s->c1     = c16(c1);
    s->frame  = c16(frame);
    s_head = (s_head + 1) % HIST;
}

#define COL_PANEL  MOTE_RGB565(8, 8, 14)
#define COL_UPDATE MOTE_RGB565(230, 90, 90)    /* game update (physics/logic) */
#define COL_RASTER MOTE_RGB565(90, 210, 110)   /* raster (both cores, parallel) */
#define COL_FLUSH  MOTE_RGB565(90, 150, 240)   /* LCD flush */
#define COL_REF    MOTE_RGB565(80, 80, 95)      /* 60fps reference line */
#define COL_TEXT   MOTE_RGB565(235, 235, 120)

static void fillrect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= MOTE_FB_H) continue;
        uint16_t *row = fb + j * MOTE_FB_W;
        for (int i = x; i < x + w; i++)
            if ((unsigned)i < MOTE_FB_W) row[i] = c;
    }
}

/* A vertical bar of `h` px ending at baseline `yb` (exclusive), at column x. */
static int vbar(uint16_t *fb, int x, int yb, int h, uint16_t c) {
    for (int k = 0; k < h; k++) {
        int y = yb - 1 - k;
        if (y < 0) break;
        if ((unsigned)x < MOTE_FB_W) fb[y * MOTE_FB_W + x] = c;
    }
    return yb - h;
}

void mote_perf_overlay(uint16_t *fb) {
#ifdef MOTE_HOST
    static int s_checked;
    if (!s_checked) { s_checked = 1; extern char *getenv(const char *); if (getenv("MOTE_PERF")) s_on = 1; }
#endif
    if (!s_on) return;

    const int GW = HIST, GH = 34;
    const int GX = (MOTE_FB_W - GW) / 2;
    const int GY = MOTE_FB_H - GH - 2;            /* graph top */
    const int YB = GY + GH;                        /* baseline */
    const float scale = (float)GH / 33333.0f;      /* full height = 33ms (30fps) */

    fillrect(fb, GX - 1, GY - 1, GW + 2, GH + 2, COL_PANEL);

    int y60 = YB - (int)(16667.0f * scale);        /* 60fps reference */
    for (int x = GX; x < GX + GW; x++)
        if ((unsigned)y60 < MOTE_FB_H) fb[y60 * MOTE_FB_W + x] = COL_REF;

    for (int i = 0; i < HIST; i++) {
        const Sample *s = &s_hist[(s_head + i) % HIST];
        int x = GX + i, yb = YB;
        yb = vbar(fb, x, yb, (int)(s->update * scale), COL_UPDATE);
        yb = vbar(fb, x, yb, (int)(s->raster * scale), COL_RASTER);
        yb = vbar(fb, x, yb, (int)(s->flush  * scale), COL_FLUSH);
    }

    const Sample *l = &s_hist[(s_head + HIST - 1) % HIST];
    int fps   = l->frame ? (int)(1000000u / l->frame) : 0;
    int c1pct = l->frame ? (l->c1 * 100 / l->frame) : 0;
    char b[40];
    snprintf(b, sizeof b, "U%u R%u F%u us", l->update, l->raster, l->flush);
    mote_font_draw(fb, b, GX, GY - 14, COL_TEXT);
    snprintf(b, sizeof b, "%dfps  core1 %d%%", fps, c1pct);
    mote_font_draw(fb, b, GX, GY - 7, COL_TEXT);
}
