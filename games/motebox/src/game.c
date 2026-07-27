/*
 * Motebox — a god simulator for the Thumby Color.
 *
 * A living pixel world you nudge, bless and ruin. See DESIGN.md.
 *
 * Phase 1: the world and the two views. God's Eye (1 tile = 1 pixel, the whole
 * world at once) and Mortal View (8 px tiles, sprites), a cursor that survives
 * the zoom, the clock, and the HUD.
 *
 * Frame flow: update() reads input, advances the world clock by the current
 * speed, and submits whichever view is live; overlay() draws the cursor and HUD
 * over the top.
 */
#include "mb.h"
MOTE_GAME_MODULE();
MOTE_GAME_META("Motebox", "austinio7116");
MOTE_GAME_VERSION("0.1.0");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#if MOTE_HOST
#include <stdlib.h>          /* getenv — the headless test hooks */
#endif
#include <stdio.h>
#include <string.h>

#include "rogue8.font.h"

/* --- time ---------------------------------------------------------------
 * One tick is a WEEK, 52 to the year: at x1 a year takes 6.5 s and a 250-year
 * epic runs in about three and a half minutes at x8, which is the pace a
 * handheld session wants. */
#define TPY 52
static const uint8_t SPEED_TPS[4] = { 0, 8, 24, 64 };   /* ticks per second */
static const char *const SPEED_NAME[4] = { "||", "x1", "x3", "x8" };
static int s_speed = 1;
static float s_tick_acc;
#if MOTE_HOST
static int s_perf;                   /* MOTEBOX_PERF=1 */
static int s_stat;                   /* MOTEBOX_STAT=1 */
#endif

/* --- view + cursor ------------------------------------------------------ */
static int s_god = 1;                /* 1 = God's Eye, 0 = Mortal View */
static int s_cx = MW / 2, s_cy = MH / 2;
static int s_cam_x, s_cam_y;
static float s_hold;                 /* how long the d-pad has been held */
static float s_move_acc;

/* --- HUD ---------------------------------------------------------------- */
#define HUD_Y   VIEW_H
#define C_HUDBG MOTE_RGB565( 12,  14,  26)
#define C_TEXT  MOTE_RGB565(194, 195, 199)
#define C_HI    MOTE_RGB565(255, 236,  39)
#define C_WARN  MOTE_RGB565(255,   0,  77)
#define C_CURS  MOTE_RGB565(255, 241, 232)
#define C_DARK  MOTE_RGB565( 29,  43,  83)

static const char *const B_NAME[B_N] = {
    "-", "OCEAN", "SEA", "SHOAL", "ICE", "BEACH", "DESERT", "SAVANNA", "GRASS",
    "SWAMP", "HILL", "MOUNTAIN", "PEAK", "TUNDRA", "SNOW", "ASH", "SCORCHED",
    "LAVA", "ACID", "FARM", "RUBBLE", "MEADOW", "FOREST", "ROAD",
};
static const char *const O_NAME[O_N] = {
    "", "tree", "tree", "dead tree", "bush", "grass", "rock", "cactus",
    "flower", "iron", "silver", "gold", "gems", "boulder", "crag",
};

/* --- helpers ----------------------------------------------------------- */

/* Mortal View camera: centre on the cursor, clamped so the view never leaves
 * the world. Cursor position is shared between the views, so a zoom lands where
 * you were looking. */
static void cam_follow(void)
{
    s_cam_x = s_cx * TILE - 128 / 2 + TILE / 2;
    s_cam_y = s_cy * TILE - VIEW_H / 2 + TILE / 2;
    int maxx = MW * TILE - 128, maxy = MH * TILE - VIEW_H;
    if (s_cam_x < 0) s_cam_x = 0; if (s_cam_x > maxx) s_cam_x = maxx;
    if (s_cam_y < 0) s_cam_y = 0; if (s_cam_y > maxy) s_cam_y = maxy;
}

static void view_set(int god)
{
    s_god = god;
    /* The whole view switch: the world rasteriser IS the background pass, so
     * handing it over (or handing back NULL) is all that changes. */
    mote->set_background_cb(god ? mb_god_band : 0);
    if (!god) cam_follow();
}

/* --- vtbl -------------------------------------------------------------- */

static void g_init(void)
{
    uint32_t seed = 0x1d0f2c31u;
#if MOTE_HOST
    const char *e = getenv("MOTEBOX_SEED");
    if (e && *e) seed = (uint32_t)strtoul(e, 0, 0);
    e = getenv("MOTEBOX_PERF"); s_perf = (e && *e && *e != '0');
    e = getenv("MOTEBOX_STAT"); s_stat = (e && *e && *e != '0');
#endif
    g_api = mote;                 /* hand the api to the other TUs (see mb.h) */
    mote->set_fps_limit(30);
    mote->scene_set_background(C_HUDBG);
    mb_world_alloc();
    mb_world_gen(seed);
    mb_draw_init();
    mb_world_start(&s_cx, &s_cy);
#if MOTE_HOST
    if (s_stat) mb_world_stats();
#endif
    view_set(1);
}

static void g_update(float dt)
{
    const MoteInput *in = mote->input();

    /* --- clock --- */
    int tps = SPEED_TPS[s_speed];
    if (tps) {
        s_tick_acc += dt * (float)tps;
        int steps = (int)s_tick_acc;
        if (steps > 0) {
            if (steps > 8) steps = 8;          /* never let the sim eat the frame */
            s_tick_acc -= (float)steps;
            mb_w.tick += steps;
        }
    }

    /* --- cursor: accelerates while held, so crossing the world is quick but a
     * single tap is still one tile --- */
    int dx = mote_pressed(in, MOTE_BTN_RIGHT) - mote_pressed(in, MOTE_BTN_LEFT);
    int dy = mote_pressed(in, MOTE_BTN_DOWN)  - mote_pressed(in, MOTE_BTN_UP);
    if (dx || dy) {
        int first = (s_hold == 0.0f);
        s_hold += dt;
        float rate = s_hold < 0.35f ? 0.0f : (s_hold < 0.9f ? 12.0f : 34.0f);
        s_move_acc += rate * dt;
        int steps = first ? 1 : (int)s_move_acc;
        if (steps > 0) {
            s_move_acc -= (float)steps;
            s_cx += dx * steps; s_cy += dy * steps;
            if (s_cx < 0) s_cx = 0; if (s_cx >= MW) s_cx = MW - 1;
            if (s_cy < 0) s_cy = 0; if (s_cy >= MH) s_cy = MH - 1;
        }
    } else {
        s_hold = 0.0f; s_move_acc = 0.0f;
    }

    /* --- LB tap cycles speed, RB tap toggles the zoom --- */
    if (mote_just_pressed(in, MOTE_BTN_LB)) s_speed = (s_speed + 1) & 3;
    if (mote_just_pressed(in, MOTE_BTN_RB)) view_set(!s_god);

    if (!s_god) { cam_follow(); mb_draw_mortal(s_cam_x, s_cam_y); }
    else        { mote->scene2d_begin(0, 0); }   /* nothing but the background */
}

static void g_overlay(uint16_t *fb)
{
    char buf[40];
    int year = (int)(mb_w.tick / TPY);

#if MOTE_HOST
    /* MOTEBOX_PERF=1 times the world rasteriser, so a change to it (or to the sim
     * feeding it) is MEASURED rather than eyeballed. The engine's perf() counters
     * are device-side, so on the host we time the pass ourselves — one extra
     * full-frame call of the SAME function the background pass just ran, on core
     * 0, which repaints identical pixels and so leaves the frame untouched. Only
     * in God's Eye: in Mortal View it would paint over the tile view.
     *
     * The absolute figure is HOST speed, not device speed (the device number
     * needs `mote push` and real hardware) — but it is the right regression
     * tripwire for the one pass that grows with every later phase. */
    if (s_perf) {
        static uint64_t acc; static int n;
        if (s_god) {
            uint64_t t0 = mote->micros();
            mb_god_band(fb, 0, VIEW_H);
            acc += mote->micros() - t0;
        }
        if ((++n & 63) == 0) {
            uint32_t p[6]; mote->perf(p);
            fprintf(stderr, "perf %s fps=%u raster=%uus update=%uus  god_band=%.1fus\n",
                    s_god ? "GOD " : "LAND", p[0], p[2], p[1], (double)acc / 64.0);
            acc = 0;
        }
    }
#endif

    /* --- cursor ---
     * Drawn as a box AROUND the target so the tile itself still shows, and in
     * TWO TONES — dark outside, light inside. A single light box disappeared
     * against beach and snow, which is most of a polar coastline; two tones read
     * on every one of the 23 biome colours without a blink to wait for. */
    if (s_god) {
        mote->draw_rect(fb, s_cx - 3, s_cy - 3, 7, 7, C_DARK, 0, 0, VIEW_H);
        mote->draw_rect(fb, s_cx - 2, s_cy - 2, 5, 5, C_CURS, 0, 0, VIEW_H);
        mote->draw_pixel(fb, s_cx, s_cy, C_HI);
    } else {
        int px = s_cx * TILE - s_cam_x, py = s_cy * TILE - s_cam_y;
        mote->draw_rect(fb, px - 2, py - 2, TILE + 4, TILE + 4, C_DARK, 0, 0, VIEW_H);
        mote->draw_rect(fb, px - 1, py - 1, TILE + 2, TILE + 2, C_CURS, 0, 0, VIEW_H);
    }

    /* --- HUD --- */
    mote->draw_rect(fb, 0, HUD_Y, 128, 128 - HUD_Y, C_HUDBG, 1, 0, 128);
    snprintf(buf, sizeof buf, "%s Y%d", SPEED_NAME[s_speed], year);
    mote->text_font(fb, &rogue8, buf, 1, HUD_Y, C_HI);
    snprintf(buf, sizeof buf, "%s", s_god ? "GOD" : "LAND");
    mote->text_font(fb, &rogue8, buf, 96, HUD_Y, C_TEXT);

    uint8_t b = mb_w.biome[AT(s_cx, s_cy)];
    uint8_t o = mb_w.obj[AT(s_cx, s_cy)];
    if (o && o < O_N && O_NAME[o][0])
        snprintf(buf, sizeof buf, "%s %s", B_NAME[b < B_N ? b : 0], O_NAME[o]);
    else
        snprintf(buf, sizeof buf, "%s %d,%d", B_NAME[b < B_N ? b : 0], s_cx, s_cy);
    mote->text_font(fb, &rogue8, buf, 1, HUD_Y + 8, C_TEXT);

}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        /* Pure 2D: no triangles, no depth buffer. The sprite pool covers the
         * visible object grid (18x16 worst case) plus the units, FX particles
         * and emotes the later phases add. */
        .max_sprites = 320,
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
