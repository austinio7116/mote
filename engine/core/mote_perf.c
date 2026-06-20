/*
 * Mote — performance overlay.
 */
#include "mote_perf.h"
#include "mote_config.h"
#include "mote_font.h"
#include <stdio.h>

#define HIST 100

typedef struct { uint32_t update, c0, c1, flush, frame; } Sample;

static Sample s_hist[HIST];
static int    s_head;
static int    s_level;       /* 0..3 — see MOTE_PERF_* */

void mote_perf_set_level(int l) { if (l < 0) l = 0; if (l >= MOTE_PERF_LEVELS) l = MOTE_PERF_LEVELS - 1; s_level = l; }
int  mote_perf_level(void)   { return s_level; }
void mote_perf_toggle(void)  { s_level = (s_level + 1) % MOTE_PERF_LEVELS; }
int  mote_perf_enabled(void) { return s_level > 0; }

void mote_perf_get(uint32_t out[6]) {
    const Sample *l = &s_hist[(s_head + HIST - 1) % HIST];
    /* TRUE per-core utilisation over the whole frame: core0 runs the frame
     * serially and only idles while waiting on the DMA flush, so its busy
     * fraction is (frame - flush)/frame — this counts update + raster + the
     * splat pass + overlay. core1 only works the banded passes (c0/c1 already
     * include the dual-core splat blit). */
    int c0b = (int)l->frame - (int)l->flush; if (c0b < 0) c0b = 0;
    out[0] = l->frame ? 1000000u / l->frame : 0;                 /* fps */
    out[1] = l->update;                                          /* update us */
    out[2] = l->c0 > l->c1 ? l->c0 : l->c1;                      /* raster us */
    out[3] = l->flush;                                           /* flush us */
    out[4] = l->frame ? (c0b > (int)l->frame ? 100 : c0b * 100 / l->frame) : 0;
    out[5] = l->frame ? l->c1 * 100 / l->frame : 0;
}

static uint32_t s_mem_used, s_mem_total;
void mote_perf_set_mem(uint32_t used_kb, uint32_t total_kb) { s_mem_used = used_kb; s_mem_total = total_kb; }

void mote_perf_record(uint32_t upd, uint32_t c0, uint32_t c1,
                      uint32_t flush, uint32_t frame) {
    Sample *s = &s_hist[s_head];
    s->update = upd;
    s->c0     = c0;
    s->c1     = c1;
    s->flush  = flush;
    s->frame  = frame;
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
    if (!s_checked) { s_checked = 1; extern char *getenv(const char *); if (getenv("MOTE_PERF")) s_level = MOTE_PERF_FULL; }
#endif
    if (s_level <= MOTE_PERF_OFF) return;

    const int W = MOTE_FB_W;
    const int GW = HIST, GX = (W - GW) / 2;
    char b[44];
    const Sample *l = &s_hist[(s_head + HIST - 1) % HIST];
    int raster = l->c0 > l->c1 ? l->c0 : l->c1;
    int fps  = l->frame ? (int)(1000000u / l->frame) : 0;

    /* ---------- level 1: just FPS, top-right corner ---------------------- */
    if (s_level == MOTE_PERF_FPS) {
        snprintf(b, sizeof b, "%d FPS", fps);
        int w = mote_font_width(b);
        fillrect(fb, W - w - 4, 1, w + 3, 9, COL_PANEL);
        mote_font_draw(fb, b, W - w - 2, 2, COL_TEXT);
        return;
    }

    /* ---------- graph 2: frame time / phase split (0..33ms) — lvl >= 2 --- */
    const int FT_H = 26, FT_Y = MOTE_FB_H - FT_H - 2, FT_B = FT_Y + FT_H;
    const float scale = (float)FT_H / 33333.0f;
    fillrect(fb, GX - 1, FT_Y - 1, GW + 2, FT_H + 2, COL_PANEL);
    int y60 = FT_B - (int)(16667.0f * scale);
    for (int x = GX; x < GX + GW; x++)
        if ((unsigned)y60 < MOTE_FB_H) fb[y60 * W + x] = COL_REF;
    for (int i = 0; i < HIST; i++) {
        const Sample *s = &s_hist[(s_head + i) % HIST];
        int x = GX + i, yb = FT_B;
        int r = s->c0 > s->c1 ? s->c0 : s->c1;
        yb = vbar(fb, x, yb, (int)(s->update * scale), COL_UPDATE);
        yb = vbar(fb, x, yb, (int)(r          * scale), COL_RASTER);
        yb = vbar(fb, x, yb, (int)(s->flush   * scale), COL_FLUSH);
    }
    snprintf(b, sizeof b, "%dfps U%u R%u F%u", fps, l->update, raster, l->flush);
    mote_font_draw(fb, b, GX, FT_Y - 7, COL_TEXT);
    if (s_mem_total) {
        snprintf(b, sizeof b, "ARENA %u/%uK", s_mem_used, s_mem_total);
        mote_font_draw(fb, b, GX, FT_Y + 2, COL_TEXT);
    }
    if (s_level < MOTE_PERF_FULL) return;

    /* ---------- level 3: + CPU core utilisation graph -------------------- */
    const int CU_Y = 64, CU_H = 22, CU_B = CU_Y + CU_H;
    fillrect(fb, GX - 1, CU_Y - 1, GW + 2, CU_H + 2, COL_PANEL);
    for (int x = GX; x < GX + GW; x++) {        /* 100% (top) + 50% ref */
        fb[CU_Y * W + x] = COL_REF;
        fb[(CU_Y + CU_H / 2) * W + x] = COL_REF;
    }
    for (int i = 0; i < HIST; i++) {
        const Sample *s = &s_hist[(s_head + i) % HIST];
        if (!s->frame) continue;
        int x = GX + i;
        int c0b = (int)s->frame - (int)s->flush; if (c0b < 0) c0b = 0;
        int c0 = c0b * 100 / s->frame; if (c0 > 100) c0 = 100;
        int c1 = s->c1 * 100 / s->frame; if (c1 > 100) c1 = 100;
        int y0 = CU_B - 1 - c0 * (CU_H - 1) / 100;
        int y1 = CU_B - 1 - c1 * (CU_H - 1) / 100;
        fb[y0 * W + x] = COL_UPDATE;            /* core0 (red) */
        fb[y1 * W + x] = COL_RASTER;            /* core1 (green) */
    }
    int c0b  = (int)l->frame - (int)l->flush; if (c0b < 0) c0b = 0;
    int c0pct = l->frame ? (c0b > (int)l->frame ? 100 : c0b * 100 / l->frame) : 0;
    int c1pct = l->frame ? (l->c1 * 100 / l->frame) : 0;
    snprintf(b, sizeof b, "CPU C0:%d C1:%d", c0pct, c1pct);
    mote_font_draw(fb, b, GX, CU_Y - 7, COL_TEXT);
}
