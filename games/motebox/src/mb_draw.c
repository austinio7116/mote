/*
 * Motebox — the two views.
 *
 * GOD'S EYE   set_background_cb(mb_god_band): a whole-world rasteriser run on
 *             BOTH cores with disjoint row bands. One tile is one pixel, so the
 *             128x112 world is the frame; no camera, no sprite pool, no scroll.
 *
 * MORTAL VIEW scene2d_set_autotiles(biome, ...) + sprites. The engine autotiles
 *             the LIVE biome array with no resolved buffer, so terrain the sim
 *             rewrites every tick costs nothing extra to draw.
 *
 * Switching is set_background_cb(fn) vs (NULL) — both paths are the engine's
 * standard raster, both are dual-core and both show in the perf meter.
 */
#include "mb.h"

/* the baked biome rulesets — one per B_* id, in enum order */
#include "bio_ocean.tiles.h"
#include "bio_sea.tiles.h"
#include "bio_shallow.tiles.h"
#include "bio_ice.tiles.h"
#include "bio_beach.tiles.h"
#include "bio_desert.tiles.h"
#include "bio_savanna.tiles.h"
#include "bio_grass.tiles.h"
#include "bio_swamp.tiles.h"
#include "bio_hill.tiles.h"
#include "bio_mountain.tiles.h"
#include "bio_peak.tiles.h"
#include "bio_tundra.tiles.h"
#include "bio_snow.tiles.h"
#include "bio_ash.tiles.h"
#include "bio_scorched.tiles.h"
#include "bio_lava.tiles.h"
#include "bio_acid.tiles.h"
#include "bio_farm.tiles.h"
#include "bio_rubble.tiles.h"
#include "floor_jungle.tiles.h"      /* B_MEADOW */
#include "hedge.tiles.h"             /* B_FOREST */
#include "floor_cobble.tiles.h"      /* B_ROAD   */

#include "nature.h"
#include "boulders.h"
#include "treasure_ore.h"

/* Ruleset per biome id. Index i is biome id i+1 — the engine reads
 * tiles[t-1] for cell value t, so this array's ORDER IS the B_* enum. */
static const MoteAutotile *const MB_TILES[B_COUNT] = {
    &bio_ocean_at, &bio_sea_at, &bio_shallow_at, &bio_ice_at, &bio_beach_at,
    &bio_desert_at, &bio_savanna_at, &bio_grass_at, &bio_swamp_at, &bio_hill_at,
    &bio_mountain_at, &bio_peak_at, &bio_tundra_at, &bio_snow_at, &bio_ash_at,
    &bio_scorched_at, &bio_lava_at, &bio_acid_at, &bio_farm_at, &bio_rubble_at,
    &floor_jungle_at, &hedge_at, &floor_cobble_at,
};

/* God's Eye colour per biome: the BASE colour of that biome's tile recipe, so
 * the two views agree — zooming in never recolours the ground. Kept as one table
 * next to the tileset list, because the pair has to move together. */
#define C_NAVY    MOTE_RGB565( 29,  43,  83)
#define C_MAROON  MOTE_RGB565(126,  37,  83)
#define C_DKGREEN MOTE_RGB565(  0, 135,  81)
#define C_BROWN   MOTE_RGB565(171,  82,  54)
#define C_DKGREY  MOTE_RGB565( 95,  87,  79)
#define C_LTGREY  MOTE_RGB565(194, 195, 199)
#define C_WHITE   MOTE_RGB565(255, 241, 232)
#define C_RED     MOTE_RGB565(255,   0,  77)
#define C_ORANGE  MOTE_RGB565(255, 163,   0)
#define C_YELLOW  MOTE_RGB565(255, 236,  39)
#define C_GREEN   MOTE_RGB565(  0, 228,  54)
#define C_BLUE    MOTE_RGB565( 41, 173, 255)
#define C_SLATE   MOTE_RGB565(131, 118, 156)
#define C_PEACH   MOTE_RGB565(255, 204, 170)

static const uint16_t MB_COL[B_COUNT] = {
    C_NAVY,    /* ocean    */ C_BLUE,   /* sea      */ C_BLUE,   /* shallow  */
    C_WHITE,   /* ice      */ C_PEACH,  /* beach    */ C_YELLOW, /* desert   */
    C_ORANGE,  /* savanna  */ C_DKGREEN,/* grass    */ C_DKGREEN,/* swamp    */
    C_BROWN,   /* hill     */ C_DKGREY, /* mountain */ C_LTGREY, /* peak     */
    C_SLATE,   /* tundra   */ C_WHITE,  /* snow     */ C_DKGREY, /* ash      */
    C_MAROON,  /* scorched */ C_RED,    /* lava     */ C_GREEN,  /* acid     */
    C_BROWN,   /* farm     */ C_LTGREY, /* rubble   */ C_DKGREEN,/* meadow   */
    C_DKGREEN, /* forest   */ C_DKGREY, /* road     */
};

/* Object dots in God's Eye. 0 = draw the biome colour (the object is too small
 * to matter at 1 px); anything else overrides the pixel, which is how ore veins
 * and treelines show up on the political map. */
static const uint16_t MB_OBJ_COL[O_N] = {
    0,                       /* none      */
    C_GREEN, C_GREEN,        /* tree      */
    C_BROWN,                 /* dead      */
    C_GREEN,                 /* bush      */
    0,                       /* tuft      */
    0,                       /* rock      */
    C_DKGREEN,               /* cactus    */
    0,                       /* flower    */
    C_BROWN,                 /* ore       */
    C_LTGREY,                /* silver    */
    C_YELLOW,                /* gold      */
    C_RED,                   /* gem       */
    0, 0,                    /* boulder, peak rock */
};

uint16_t mb_biome_colour(uint8_t b) {
    return (b >= 1 && b <= B_COUNT) ? MB_COL[b - 1] : 0;
}

void mb_draw_init(void) { }

/* ------------------------------------------------------------ God's Eye ---
 * The engine hands us LOGICAL row bands; on device MOTE_SS == 1 so logical and
 * physical coincide, but the host screenshot tooling can supersample, so expand
 * through one helper rather than assuming a stride. */
static inline void px_put(uint16_t *fb, int x, int y, uint16_t c)
{
#if MOTE_SS == 1
    fb[y * MOTE_FB_PW + x] = c;
#else
    for (int sy = 0; sy < MOTE_SS; sy++)
        for (int sx = 0; sx < MOTE_SS; sx++)
            fb[(y * MOTE_SS + sy) * MOTE_FB_PW + x * MOTE_SS + sx] = c;
#endif
}

void mb_god_band(uint16_t *fb, int y0, int y1)
{
    const uint8_t *bio = mb_w.biome, *ob = mb_w.obj;
    if (y0 < 0) y0 = 0;
    if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;

    for (int y = y0; y < y1; y++) {
        if (y >= VIEW_H) {                       /* HUD strip: overlay owns it */
            for (int x = 0; x < MW; x++) px_put(fb, x, y, C_NAVY);
            continue;
        }
        const uint8_t *brow = bio + y * MW;
        const uint8_t *orow = ob  + y * MW;
        for (int x = 0; x < MW; x++) {
            uint8_t b = brow[x];
            uint16_t c = (b >= 1 && b <= B_COUNT) ? MB_COL[b - 1] : 0;
            uint8_t o = orow[x];
            if (o && o < O_N) { uint16_t oc = MB_OBJ_COL[o]; if (oc) c = oc; }
            px_put(fb, x, y, c);
        }
    }
}

/* --------------------------------------------------------- Mortal View ---
 * Object sprite cells, (sheet, fx, fy). The sheets keep the master's layout, so
 * these coordinates are the ones the labelled contact sheets show.
 */
typedef struct { const MoteImage *img; uint8_t cx, cy; } ObjSpr;
static const ObjSpr MB_OBJ_SPR[O_N] = {
    { 0, 0, 0 },                        /* none      */
    { &nature_img,        6, 4 },       /* tree      */
    { &nature_img,        7, 4 },       /* tree2     */
    { &nature_img,        8, 4 },       /* dead tree */
    { &nature_img,        4, 4 },       /* bush      */
    { &nature_img,        2, 4 },       /* tuft      */
    { &nature_img,        5, 4 },       /* rock      */
    { &nature_img,        9, 3 },       /* cactus    */
    { &nature_img,        0, 3 },       /* flower    */
    { &treasure_ore_img,  1, 2 },       /* ore (copper pile)  */
    { &treasure_ore_img,  1, 1 },       /* silver    */
    { &treasure_ore_img,  1, 0 },       /* gold      */
    { &treasure_ore_img,  7, 0 },       /* gem       */
    { &boulders_img,      0, 1 },       /* boulder   */
    { &boulders_img,      4, 1 },       /* snow peak */
};

void mb_draw_mortal(int cam_x, int cam_y)
{
    g_api->scene2d_begin(cam_x, cam_y);
    g_api->scene2d_set_autotiles(mb_w.biome, MW, MH, MB_TILES, B_COUNT);

    /* One sprite per visible object. The visible window is 16x14 tiles plus a
     * one-tile margin for partial scroll, so this is bounded at ~18x16 = 288
     * whatever the world does. */
    int c0 = cam_x / TILE, r0 = cam_y / TILE;
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t o = mb_w.obj[AT(c, r)];
            if (!o || o >= O_N) continue;
            const ObjSpr *s = &MB_OBJ_SPR[o];
            if (!s->img) continue;
            MoteSprite spr = {
                s->img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                (uint16_t)(s->cx * TILE), (uint16_t)(s->cy * TILE), TILE, TILE,
                /* layer: draw order is height order, so ground clutter sits
                 * under anything that walks (units land on layer 20+). */
                10, 0
            };
            g_api->scene2d_add(&spr);
        }
    }
}
