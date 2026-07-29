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
#include "ore.h"
#include "road.h"
#include "bd_water.tiles.h"
#include "bd_frost.tiles.h"
#include "bd_sand.tiles.h"
#include "bd_desert.tiles.h"
#include "bd_green.tiles.h"
#include "bd_savanna.tiles.h"
#include "bd_dry.tiles.h"
#include "bd_rock.tiles.h"
#include "blob47_lut.h"
#include "tr_ocean.tiles.h"
#include "tr_sea.tiles.h"
#include "tr_shallow.tiles.h"
#include "tr_ice.tiles.h"
#include "tr_beach.tiles.h"
#include "tr_desert.tiles.h"
#include "tr_savanna.tiles.h"
#include "tr_grass.tiles.h"
#include "tr_swamp.tiles.h"
#include "tr_hill.tiles.h"
#include "tr_mountain.tiles.h"
#include "tr_peak.tiles.h"
#include "tr_tundra.tiles.h"
#include "tr_snow.tiles.h"
#include "tr_ash.tiles.h"
#include "tr_scorched.tiles.h"
#include "tr_lava.tiles.h"
#include "tr_acid.tiles.h"
#include "tr_farm.tiles.h"
#include "tr_rubble.tiles.h"
#include "town.h"
#include "mtn.h"
#include "hot.h"

/* The town sheet's geometry, in one place: 8 wide, 14 tall, drawn six pixels above
 * its own tile. Must match authoring/build_sprites.py. */
#define TOWN_W 8
#define TOWN_H 14
#define TOWN_ANCHOR_Y (-6)
/* The sheet's column IS `obj - O_BUILD0`, which is fast and has no lookup table to
 * drift — but it means adding an O_* without adding a column silently draws the wrong
 * building for every id after it. This catches exactly that, at compile time. */
typedef char mb_town_covers_builds[(town_W / TOWN_W) == (O_N - O_BUILD0) ? 1 : -1];
#include "ui_status.h"
#include "tools.h"
#include "crowns_fx.h"
#include "fx_frost.h"
#include "fx_acid.h"
#include "characters.h"
#include "animals.h"
#include "monsters.h"
#include "buildings.h"
#include "props.h"
#include "blueprint.h"

/* Ruleset per biome id. Index i is biome id i+1 — the engine reads
 * tiles[t-1] for cell value t, so this array's ORDER IS the B_* enum. */
/* THE BAND STACK, bottom-up. One tileset per autotile layer; the cumulative band map in
 * mb_w.layer decides which layers cover which cell (see mb_bands_rebuild). This replaces
 * the per-biome opaque autotile AND the per-cell sprite transition pass: every boundary
 * in the world is now drawn by the ruleset system, for zero sprites. */
static const MoteAutotile *const MB_BANDS[8] = {
    &bd_water_at, &bd_frost_at, &bd_sand_at, &bd_desert_at,
    &bd_green_at, &bd_savanna_at, &bd_dry_at, &bd_rock_at,
};

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
/* The GRASS BAND is a third of the way to bright green so the artist's dark foliage
 * reads against it — God's Eye has to use the same tone or zooming recolours the world. */
#define C_GRASS   MOTE_RGB565(  0, 163,  73)
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

/* TERRAIN TRANSITIONS: which sheet each biome bleeds with, and how high it sits.
 *
 * A cell may only paint its own eight pixels, so in one opaque layer the colour
 * boundary is pinned to the tile grid and every coast is a staircase of straight
 * segments. The base pass paints flat fields; this table drives a SECOND pass that
 * paints the boundary in the HIGHER terrain's colours inside the LOWER terrain's cell,
 * so the visible edge is organic and owes nothing to the grid.
 *
 * Precedence is a height in a stack — water at the bottom, rock at the top, which is
 * also the order they lie in reality. Only the higher one bleeds, so a boundary is
 * drawn exactly once and two neighbours can never both claim the same pixels. */
static const MoteImage *const MB_TRANS[B_COUNT] = {
    &tr_ocean_img, &tr_sea_img, &tr_shallow_img, &tr_ice_img, &tr_beach_img, &tr_desert_img, &tr_savanna_img, &tr_grass_img, &tr_swamp_img, &tr_hill_img, &tr_mountain_img, &tr_peak_img, &tr_tundra_img, &tr_snow_img, &tr_ash_img, &tr_scorched_img, &tr_lava_img, &tr_acid_img, &tr_farm_img, &tr_rubble_img,
    0, 0, 0,   /* meadow, forest and road are synced rulesets: no overlay */
};

static const uint8_t MB_PREC[B_COUNT] = {
    0, 1, 2, 3, 6, 7, 8, 10, 5, 16, 17, 18, 12, 11, 13, 14, 4, 4, 9, 15,
    10, 11, 9,   /* meadow, forest, road */
};
typedef char mb_trans_covers[(sizeof MB_TRANS / sizeof MB_TRANS[0]) == B_COUNT ? 1 : -1];
typedef char mb_prec_covers[(sizeof MB_PREC / sizeof MB_PREC[0]) == B_COUNT ? 1 : -1];

static const uint16_t MB_COL[B_COUNT] = {
    C_NAVY,    /* ocean    */ C_BLUE,   /* sea      */ C_BLUE,   /* shallow  */
    C_WHITE,   /* ice      */ C_PEACH,  /* beach    */ C_YELLOW, /* desert   */
    C_ORANGE,  /* savanna  */ C_GRASS,  /* grass    */ C_GRASS,  /* swamp    */
    C_BROWN,   /* hill     */ C_DKGREY, /* mountain */ C_LTGREY, /* peak     */
    C_BROWN,   /* tundra   */ C_WHITE,  /* snow     */ C_DKGREY, /* ash      */
    C_MAROON,  /* scorched */ C_RED,    /* lava     */ C_GREEN,  /* acid     */
    C_BROWN,   /* farm     */ C_LTGREY, /* rubble   */ C_GRASS,  /* meadow   */
    C_GRASS,   /* forest   */ C_DKGREY, /* road     */
};

/* Object dots in God's Eye. 0 = draw the biome colour (the object is too small
 * to matter at 1 px); anything else overrides the pixel, which is how ore veins
 * and treelines show up on the political map. */
/* MOST OBJECTS DRAW NOTHING at one pixel per tile, and that is the point. The first
 * version gave trees bright green, ore brown, silver light grey, gold yellow and gems
 * red — so a continent was strewn with confetti and the important marks (a
 * settlement, a fire, an army) had to compete with a thousand pebbles. The biome
 * colour already says "forest"; only what a player would ACT on gets a pixel. */
static const uint16_t MB_OBJ_COL[O_N] = {
    0,                       /* none      */
    0, 0,                    /* tree, tree2 — the forest biome already reads green */
    0,                       /* dead tree */
    0,                       /* bush      */
    0,                       /* tuft      */
    0,                       /* rock      */
    0,                       /* cactus    */
    0,                       /* flower    */
    0,                       /* iron      — too common to be news */
    0,                       /* silver    */
    C_ORANGE,                /* gold      — worth crossing a map for */
    C_MAROON,                /* gems      */
    0, 0,                    /* boulder, crag */
    C_SLATE,                 /* grave     — a battlefield should be visible */
};

/* The five banner colours the buildings sheet is drawn in — so a kingdom's tint
 * on the political map is the SAME colour as its houses when you zoom in. */
static const uint16_t KING_COL[5] = {
    MOTE_RGB565(255, 119, 168),   /* pink   */
    MOTE_RGB565( 41, 173, 255),   /* blue   */
    MOTE_RGB565(255, 236,  39),   /* gold   */
    MOTE_RGB565(  0, 228,  54),   /* green  */
    MOTE_RGB565(194, 195, 199),   /* white  */
};
uint16_t mb_kingdom_colour(int k)
{
    return (k > 0 && k < MAXK && mb_k[k].alive) ? KING_COL[mb_k[k].colour % 5] : 0;
}

/* Blend two RGB565s by an 0-255 amount, channel-wise. Two shifts and a multiply
 * per channel: cheap enough to run on every claimed pixel of the world. */
static inline uint16_t blend565(uint16_t a, uint16_t b, int amt)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (((br - ar) * amt) >> 8);
    int g = ag + (((bg - ag) * amt) >> 8);
    int bl = ab + (((bb - ab) * amt) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Flux colour in God's Eye, per kind. Fire and lava ALTERNATE between two stops
 * on a parity of (tick + cell), so a burning front visibly crackles at one pixel
 * per tile — the cheapest possible animation, and the thing that makes a fire
 * read as alive rather than as a red stain. */
static const uint16_t FLUX_HI[FX_N] = { 0, C_YELLOW, C_ORANGE, C_WHITE, C_GREEN,  C_WHITE };
static const uint16_t FLUX_LO[FX_N] = { 0, C_RED,    C_RED,    C_BLUE,  C_YELLOW, C_BLUE  };

uint16_t mb_biome_colour(uint8_t b) {
    return (b >= 1 && b <= B_COUNT) ? MB_COL[b - 1] : 0;
}

/* --- the tint LUT -------------------------------------------------------
 * The band pass used to blend per pixel: political tint, then the age wash, six
 * shifts and three multiplies each, 14336 times a frame. Measured, that took the
 * God's Eye pass from 20 us to 83 us on the host — a 4x cost for two colours that
 * only change when a kingdom or an age does.
 *
 * So it is a table instead: for every (owner, biome) pair, the finished colour,
 * once for the interior and once for a border cell. 13 x 23 x 2 entries is 1.2 KB
 * of RAM and 598 blends per rebuild, against 43,008 per frame. */
static uint16_t s_lut_in[MAXK + 1][B_COUNT];
static uint16_t s_lut_edge[MAXK + 1][B_COUNT];
static int      s_lut_age = -1, s_lut_tint = -1;
static uint32_t s_lut_sig;
/* village id -> kingdom id, so the border test is four array reads instead of four
 * calls into mb_kingdom_of() with its bounds checks. Those four calls per claimed
 * pixel were the whole remaining cost after the colour table went in. */
static uint8_t  s_kof[MAXV];

void mb_draw_prepare(void)
{
    int amt; uint16_t age_col = mb_age_tint(&amt);
    int tint = mb_law(LAW_TINT);
    /* a signature over what the table depends on, so it rebuilds only on a change */
    uint32_t sig = (uint32_t)mb_age_id() * 131u + (uint32_t)tint * 7u;
    for (int k = 1; k < MAXK; k++)
        sig = sig * 31u + (uint32_t)(mb_k[k].alive ? (mb_k[k].colour + 1) : 0);
    for (int v = 0; v < MAXV; v++)
        s_kof[v] = (uint8_t)((v > 0 && mb_v[v].alive) ? mb_v[v].kingdom : 0);
    if (sig == s_lut_sig && s_lut_age == mb_age_id() && s_lut_tint == tint) return;
    s_lut_sig = sig; s_lut_age = mb_age_id(); s_lut_tint = tint;

    for (int k = 0; k <= MAXK; k++) {
        uint16_t kc = (k > 0) ? mb_kingdom_colour(k) : 0;
        for (int b = 0; b < B_COUNT; b++) {
            uint16_t base = MB_COL[b];
            uint16_t in = base, ed = base;
            if (tint && kc) { in = blend565(base, kc, 40); ed = blend565(base, kc, 210); }
            if (amt) { in = blend565(in, age_col, amt); ed = blend565(ed, age_col, amt); }
            s_lut_in[k][b] = in;
            s_lut_edge[k][b] = ed;
        }
    }
}

void mb_draw_init(void) { s_lut_sig = 0; s_lut_age = -1; mb_draw_prepare(); }

/* Darken a rectangle of the framebuffer TOWARD a colour, by reading it back and
 * blending. draw_rect can only fill opaquely, so a translucent panel has to be done
 * a pixel at a time — which is fine for a UI overlay of a few thousand pixels, and it
 * is the only way to keep the world readable underneath. */
void mb_dim_rect(uint16_t *fb, int x0, int y0, int w, int h, uint16_t toward, int amt)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > MOTE_FB_W) w = MOTE_FB_W - x0;
    if (y0 + h > VIEW_H)    h = VIEW_H - y0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
#if MOTE_SS == 1
            uint16_t *p = &fb[y * MOTE_FB_PW + x];
            *p = blend565(*p, toward, amt);
#else
            for (int sy = 0; sy < MOTE_SS; sy++)
                for (int sx = 0; sx < MOTE_SS; sx++) {
                    uint16_t *p = &fb[(y * MOTE_SS + sy) * MOTE_FB_PW + x * MOTE_SS + sx];
                    *p = blend565(*p, toward, amt);
                }
#endif
        }
}

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
        const uint8_t *frow = mb_w.flux + y * MW;
        const uint8_t *crow = mb_w.claim + y * MW;
        for (int x = 0; x < MW; x++) {
            uint8_t b = brow[x];
            if (b < 1 || b > B_COUNT) { px_put(fb, x, y, 0); continue; }

            /* POLITICAL TINT — the single effect that turns a pixel grid into a map
             * you can read a war off. Two table lookups: the owner's colour washed
             * lightly through the interior and hard along the BORDER (a cell whose
             * neighbour belongs to someone else), so frontiers draw themselves with
             * no border-tracing pass and no per-pixel arithmetic. */
            int k = s_kof[crow[x] < MAXV ? crow[x] : 0];
            uint16_t c;
            if (k) {
                int edge = (x > 0      && s_kof[crow[x - 1]]  != k) ||
                           (x < MW - 1 && s_kof[crow[x + 1]]  != k) ||
                           (y > 0      && s_kof[crow[x - MW]] != k) ||
                           (y < MH - 1 && s_kof[crow[x + MW]] != k);
                c = edge ? s_lut_edge[k][b - 1] : s_lut_in[k][b - 1];
            } else {
                c = s_lut_in[0][b - 1];
            }

            uint8_t o = orow[x];
            if (o && o < O_N) {
                if (mb_is_build(o)) {
                    /* A BUILDING IS WHITE WITH A COLOURED HALL, not another dot in
                     * the kingdom's colour. At 1 px/tile a building drawn in the
                     * banner colour was indistinguishable from a person walking on
                     * claimed ground, so a village of thirty houses read as a
                     * slightly denser crowd and the answer to "where are the
                     * civilizations" was "nowhere visible". White reads as built,
                     * and only the hall carries the banner. */
                    int hall = (o == O_HALL1 || o == O_HALL2 || o == O_HALL3
                                || o == O_FIRE_PIT);
                    uint16_t kc = mb_kingdom_colour(k);
                    c = (hall && kc) ? kc : C_WHITE;
                } else {
                    uint16_t oc = MB_OBJ_COL[o];
                    if (oc) c = oc;
                }
            }

            uint8_t fk = mb_fkind(frow[x]);
            if (fk && fk < FX_N)
                c = ((x + y + (int)mb_w.tick) & 1) ? FLUX_HI[fk] : FLUX_LO[fk];

            px_put(fb, x, y, c);
        }
    }
}

/* Units in God's Eye: one pixel each, drawn AFTER the terrain band so they sit on
 * top of it, and only for the rows this band owns. A crowd of three hundred
 * pixels streaming toward a border is the whole argument for this view. */
void mb_god_units(uint16_t *fb, int y0, int y1)
{
    for (int i = 0; i < mb_nu; i++) {
        const Unit *u = &mb_u[i];
        if (!u->alive) continue;
        int x = u->x >> 4, y = u->y >> 4;
        if (y < y0 || y >= y1 || y >= VIEW_H || x < 0 || x >= MW) continue;
        uint16_t c;
        if (u->sp < SP_CIV_N) {
            /* people wear their kingdom, which is how you see an army move */
            c = mb_kingdom_colour(mb_kingdom_of(u->village));
            if (!c) c = C_WHITE;                   /* the unaligned: wanderers */
        } else {
            c = (MB_SP[u->sp].diet == DIET_MEAT) ? C_RED : C_BROWN;
        }
        px_put(fb, x, y, c);
    }
}

/* --------------------------------------------------------- Mortal View ---
 * Object sprite cells, (sheet, fx, fy). The sheets keep the master's layout, so
 * these coordinates are the ones the labelled contact sheets show.
 */
typedef struct { const MoteImage *img; uint8_t cx, cy; } ObjSpr;

/* EVERY CELL BELOW IS THE LABEL SET'S OWN NAME FOR IT. Checked one at a time after
 * a village turned out to be full of GHOSTS: the grave was drawing ui_status[11,3],
 * which the labels call "ghost icon (grey, large)". The rest of the list was no
 * better — the mine was a "station/shop" (a computer terminal), the dock was "train
 * track corner", the fire pit was "hay" and the farm was "orange flowers". */
static const ObjSpr MB_OBJ_SPR[O_N] = {
    { 0, 0, 0 },                        /* none                                  */
    { &nature_img,        6, 4 },       /* tree      "tree (foliage on trunk)"   */
    { &nature_img,        7, 4 },       /* tree2     "tree (foliage on trunk)"   */
    { &nature_img,        8, 4 },       /* dead tree "tree trunk/dead tree"      */
    { &nature_img,        4, 4 },       /* bush      "green bush"                */
    { &nature_img,        2, 4 },       /* tuft      "grass tuft"                */
    { &nature_img,        5, 4 },       /* rock      "brown rock/boulder"        */
    { &nature_img,        9, 3 },       /* reeds     "reeds" — swamp, not desert */
    { &nature_img,        0, 3 },       /* flower    "pink flower"               */
    /* ORE IS GENERATED (authoring/extract_box.py build_ore), not taken from the
     * master, because the master has none. These four used to point at treasure_ore
     * (1,0) (1,1) (1,2) (7,0), which the label set calls "gold small pile (3)",
     * "silver small pile (3)", "copper small pile (3)" and "silver ingot/bar" — so
     * the wilderness was littered with LOOSE COINS AND BARS lying in the grass. That
     * whole rectangle is treasure: a thing you pick up, not a seam you mine. */
    { &ore_img,           0, 0 },       /* iron   — rust through brown stone     */
    { &ore_img,           1, 0 },       /* silver — grey stone, a bright glint   */
    { &ore_img,           2, 0 },       /* gold                                  */
    { &ore_img,           3, 0 },       /* gems                                  */
    { &boulders_img,      0, 1 },       /* boulder   "grey boulder"              */
    { &boulders_img,      4, 1 },       /* crag      "mountain slope"            */
    { &nature_img,       15, 0 },       /* grave     "tombstone"  (was a GHOST)  */
};
/* The tripwire O_NAME has. This array was 15 entries long while the enum grew past
 * 30, so graves drew nothing at all: a battlefield with thirty-four dead showed bare
 * grass. A short array is silent, which is exactly why it needs a check. */
typedef char mb_objspr_covers_world[(sizeof MB_OBJ_SPR / sizeof MB_OBJ_SPR[0]) >= O_BUILD0 ? 1 : -1];

/* One sprite per visible flux cell. Lava needs none — it IS the biome — so the table
 * covers only the kinds that sit ON the ground rather than replacing it. */
typedef struct { const MoteImage *img; uint8_t cx, cy, frames; } FluxSpr;
static const FluxSpr FLUX_SPR[FX_N] = {
    { 0, 0, 0, 0 },                       /* none  */
    { &crowns_fx_img,  6, 7, 1 },         /* fire  — "fire/explosion frame" */
    { 0, 0, 0, 0 },                       /* lava  — drawn as terrain */
    { &fx_frost_img,  13, 3, 3 },         /* water — spray */
    { &fx_acid_img,   13, 2, 3 },         /* acid  — hissing sparkle */
    { &fx_frost_img,  12, 4, 3 },         /* frost — twinkle */
};

/* BUILDINGS, every cell confirmed against the label set.
 *
 * The buildings sheet is FOUR COLUMNS BY FIVE COLOUR ROWS: col 0 a full-tile walled
 * face with a window, col 1 the same with the door open, col 2 a house with a
 * narrower pitched roof, col 3 its wide continuation. The row is the kingdom's banner
 * colour, so a village is visibly one kingdom's village.
 *
 * THE HALL IS THE SOLID BLOCK, THE HOUSE IS THE PITCHED ROOF — that is what lets you
 * tell the centre of a village from its outskirts at eight pixels. */
typedef struct { uint8_t sheet, cx, cy; } BldSpr;
enum { BS_BUILD = 0, BS_PROPS, BS_PLAN, BS_NATURE, BS_TOOLS };
static const BldSpr MB_BLD[O_N - O_BUILD0] = {
    { BS_NATURE,12, 0 },   /* fire pit  "fountain/well" — a village well marks a
                            * founding better than the "hay" this used to be     */
    /* THE FOUR COLUMNS ARE THE WHOLE VOCABULARY, so spend them: every tier that
     * shares a cell with another tier is a tier you cannot see. hall2 and hall3 were
     * both column 0 and house1 and house2 were both column 2, so half the ladder was
     * invisible — a village that had grown into a castle looked no different from one
     * that had a great hall, which is most of the reward for playing. */
    { BS_BUILD,  2, 0 },   /* hall 1    — still just a big house at tier one       */
    { BS_BUILD,  0, 0 },   /* hall 2    — a walled block: it grew                  */
    { BS_BUILD,  1, 0 },   /* hall 3    — the block with its GATE open: a keep     */
    { BS_BUILD,  2, 0 },   /* house 1   — the pitched roof                         */
    { BS_BUILD,  2, 0 },   /* house 2   — the pitched roof MIRRORED (see below)    */
    { BS_BUILD,  0, 0 },   /* house 3   — a manor is a walled house                */
    { BS_NATURE, 3, 1 },   /* farm      "hay" — a haystack, not "orange flowers"  */
    { BS_TOOLS,  0, 1 },   /* mine      "iron pickaxe", not a computer terminal   */
    { BS_NATURE, 8, 4 },   /* woodcutter"tree trunk/dead tree" — a cut stump      */
    { BS_BUILD,  0, 0 },   /* barracks  — a walled front in the kingdom's colour  */
    { BS_NATURE,11, 0 },   /* temple    "stone pillars/columns"                   */
    { BS_NATURE, 7, 0 },   /* tower     "lit torch" — a beacon                    */
    { BS_PROPS,  7, 1 },   /* dock      "wooden barrel" — goods on a quay; the
                            * master has no boat, and this was "train track corner" */
    { BS_BUILD,  1, 0 },   /* wall      — the open-door front reads as a gate     */
    /* A QUIET MARK. This was blueprint (3,2), "blueprint room (small/single cell)" —
     * a thick navy square, and with the grass drawing its bright rim around it, a
     * village's intentions were the loudest thing on the screen. A stub reads as a
     * surveyor's peg: there if you look for it, invisible if you are not. */
    { BS_PLAN,   3, 0 },   /* "blueprint wall (stub)"                             */
};

/* How many sprites the last Mortal View frame asked for, and how many the scene
 * refused. scene2d_add fails soft at MOTE_SCENE2D_MAX_SPRITES, and "the buildings are
 * missing" was that failure being silent — so it is now counted, and MOTEBOX_SPRITES=1
 * says so out loud rather than leaving the next person to census the map. */
static int s_spr_want, s_spr_lost;
void mb_draw_sprite_load(int *want, int *lost) { *want = s_spr_want; *lost = s_spr_lost; }

static void add(const MoteSprite *s)
{
    s_spr_want++;
    if (!g_api->scene2d_add(s)) s_spr_lost++;
}

void mb_draw_mortal(int cam_x, int cam_y)
{
    s_spr_want = s_spr_lost = 0;
    g_api->scene2d_begin(cam_x, cam_y);
    /* THE WHOLE TERRAIN, from the ruleset system. Deep ocean is the background colour, so
     * a cell with no bits set simply shows through — which is why the stack needs only
     * eight layers where there used to be twenty-three opaque tilesets AND a sprite per
     * boundary cell. */
    g_api->scene_set_background(MB_COL[B_OCEAN - 1]);
    g_api->scene2d_set_autotile_layers(mb_w.layer, MW, MH, MB_BANDS, 8);

    /* DRAW ORDER IS PRIORITY ORDER, not height order.
     *
     * scene2d holds MOTE_SCENE2D_MAX_SPRITES (128) and scene2d_add FAILS SOFT past
     * that. A 16x14 window is 224 tiles, so a busy screen can always ask for more
     * than the budget — and with ground clutter going in first, a settled village
     * spent the entire budget on grass and headstones, and the sprites that got
     * silently dropped were the BUILDINGS AND THE PEOPLE. A village with a castle,
     * a temple, eighteen houses and twenty citizens drew as bare ground: that is
     * what "why are there no buildings in the civilizations?" was looking at.
     *
     * So the order is what matters most first — buildings, people, disasters, flux,
     * then clutter. The `layer` field still decides what draws ON TOP of what, so
     * the picture is unchanged while there is room; past the cap you now lose a few
     * tufts of grass instead of the town. */
    int c0 = cam_x / TILE, r0 = cam_y / TILE;
    /* BUILDINGS, from the game's OWN sprite sheet (authoring/build_sprites.py), and
     * EIGHT BY FOURTEEN rather than eight by eight.
     *
     * The master's buildings rectangle offers two usable standalone fronts for a ladder
     * of six dwelling and hall tiers plus a temple, a barracks, a tower, a barn, a mine
     * and a dock — so most of the settlement shared art, and a castle looked like a
     * great hall. These are drawn instead, one column per O_* id and one row per banner
     * colour, so the column IS `o - O_BUILD0` and there is no lookup table to drift.
     *
     * The extra six pixels rise ABOVE the tile, which is the change that makes a town
     * look like a town: at eight by eight every building is a face-on box the same size
     * as a bush, and at eight by fourteen it has a pitched roof, a wall with a door in
     * it and a silhouette. Rows are added north to south and scene2d keeps insertion
     * order within a layer, so a southern roof correctly overlaps its northern
     * neighbour and a street has depth. */
    for (int r = r0; r <= r0 + MVH + 1; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t o = mb_w.obj[AT(c, r)];
            if (!mb_is_build(o)) continue;
            int col = o - O_BUILD0;
            int k = mb_kingdom_of(mb_w.claim[AT(c, r)]);
            int row = (k && mb_k[k].alive) ? mb_k[k].colour % 5 : 4;   /* 4 = unclaimed grey */
            MoteSprite spr = { &town_img, (int16_t)(c * TILE),
                               (int16_t)(r * TILE + TOWN_ANCHOR_Y),
                               (uint16_t)(col * TOWN_W), (uint16_t)(row * TOWN_H),
                               TOWN_W, TOWN_H, 30, 0 };
            add(&spr);
            /* A CAPITAL WEARS ITS CROWN — one 8x8 cell from the master's five crowns,
             * above the roof of a kingdom's seat. It is the only way to tell at a glance
             * which of forty villages is the one that matters. */
            if (o == O_HALL2 || o == O_HALL3) {
                int v = mb_w.claim[AT(c, r)];
                if (k && mb_k[k].alive && mb_k[k].capital == v) {
                    MoteSprite cr = { &crowns_fx_img, (int16_t)(c * TILE),
                                      (int16_t)(r * TILE + TOWN_ANCHOR_Y - 6),
                                      (uint16_t)((mb_k[k].colour % 5) * TILE),
                                      (uint16_t)(5 * TILE), TILE, TILE, 34, 0 };
                    add(&cr);
                }
            }
        }
    }

    /* people and beasts, above their buildings so a crowd is never hidden */
    for (int i = 0; i < mb_nu; i++) {
        const Unit *u = &mb_u[i];
        if (!u->alive) continue;
        int px = (u->x >> 4) * TILE, py = (u->y >> 4) * TILE;
        if (px < cam_x - TILE || py < cam_y - TILE ||
            px > cam_x + 128 || py > cam_y + VIEW_H) continue;
        const MbSpecies *S = &MB_SP[u->sp];
        const MoteImage *img = (S->sheet == 0) ? &characters_img
                             : (S->sheet == 1) ? &monsters_img : &animals_img;
        MoteSprite spr = { img, (int16_t)(u->x >> 4 << 3), (int16_t)(u->y >> 4 << 3),
                           (uint16_t)(S->cx * TILE), (uint16_t)(S->cy * TILE),
                           TILE, TILE, 40, 0 };
        add(&spr);
    }
    /* the walking disasters: the master's smoke swirl for a tornado, its cone
     * for a vent, lifted a tile so they stand above the ground they are wrecking */
    for (int i = 0, n = mb_agent_max(); i < n; i++) {
        int ax, ay, kind;
        if (!mb_agent_get(i, &ax, &ay, &kind)) continue;
        int cx = (kind == AG_TORNADO) ? 3 : 5, cy = (kind == AG_TORNADO) ? 7 : 5;
        MoteSprite spr = { &crowns_fx_img, (int16_t)(ax * TILE), (int16_t)(ay * TILE - TILE),
                           (uint16_t)(cx * TILE), (uint16_t)(cy * TILE), TILE, TILE, 70, 0 };
        add(&spr);
    }
    /* flux, on top of the ground it is consuming */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t k = mb_fkind(mb_w.flux[AT(c, r)]);
            if (!k || k >= FX_N || !FLUX_SPR[k].img) continue;
            const FluxSpr *f = &FLUX_SPR[k];
            int fr = f->frames > 1 ? (int)((mb_w.tick + c + r) % f->frames) : 0;
            MoteSprite spr = {
                f->img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                (uint16_t)((f->cx + fr) * TILE), (uint16_t)(f->cy * TILE), TILE, TILE,
                50, 0
            };
            add(&spr);
        }
    }
    /* LAVA AND SCORCHED, the band that would not fit. Eight layers hold eight bands and
     * there is no ninth, so the rarest pair moved to sprites — measured at 0.0-0.6% of a
     * world, about one cell in view. They always carry flux and FX anyway. */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t b = mb_w.biome[AT(c, r)];
            if (b != B_LAVA && b != B_SCORCHED) continue;
            int col = (b == B_LAVA) ? 0 : 1;
            int v = ((c * 7 + r * 3) & 1);
            MoteSprite spr = { &hot_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               (uint16_t)(col * TILE), (uint16_t)(v * TILE),
                               TILE, TILE, 13, 0 };
            add(&spr);
        }
    }

    /* (The per-cell transition sprites that used to live here are gone: the band stack
     * draws every boundary automatically, so terrain costs no sprites at all and can
     * never be starved by a passing deer.) */

    /* MOUNTAINS: the region is DECOMPOSED, core first, then its ragged edge.
     *
     * The artist's mountain is a composed 2x2 unit — cap, two alternating slope rows that
     * make diamond ridges, a foot with a snowline — and a tileset cannot place it, because
     * the engine picks a variant row with a position hash and nothing else. A sprite has
     * no grid, so the phase is chosen here.
     *
     * But a mountain region is not a grid of 2x2 blocks: its outline is ragged. The first
     * version drew a full block wherever ANY cell of an aligned 2x2 was mountain, so every
     * range grew square shoulders and spilled over its own boundary. Now the core is
     * covered by composed blocks — only where all four cells really are mountain — and
     * whatever is left over gets a single-cell crag. Core and edge, not one or the other.
     *
     * AND A MASSIF IS MOUNTAIN *AND* PEAK. Raising ground turns the high middle of a range
     * into B_PEAK and leaves B_MOUNTAIN around it, so testing for MOUNTAIN alone found
     * almost no full 2x2: the interior rendered as the bare rock band with a scatter of
     * lone crags in it, which is exactly what "the layout looks crap" was looking at. The
     * two together are one mountain as far as drawing is concerned. */
    #define MOUNTAINOUS(x, y) (mb_w.biome[AT(x, y)] == B_MOUNTAIN || \
                               mb_w.biome[AT(x, y)] == B_PEAK)
    {
        /* which cells a composed block has already covered; the window plus a margin */
        uint8_t done[MVH + 4][MVW + 4];
        memset(done, 0, sizeof done);
        #define DONE(cc, rr) done[(rr) - r0 + 1][(cc) - c0 + 1]
        int in_win = 1;

        /* PASS 1: 2x2 blocks placed GREEDILY wherever a full 2x2 of mountain fits.
         *
         * Restricting them to the global even grid was wrong: a range whose left edge
         * lands on an odd column can never use it, so the interior came out as a stripe
         * of lone crags beside a stripe of blocks. Scanning every cell and claiming as we
         * go packs the core densely and leaves only the genuinely ragged rim over. */
        for (int r = r0 - 1; r <= r0 + MVH + 1; r++) {
            for (int c = c0 - 1; c <= c0 + MVW + 1; c++) {
                if (r < 0 || c < 0 || r + 1 >= MH || c + 1 >= MW) continue;
                if (r < r0 - 1 || c < c0 - 1) continue;
                int all = 1;
                for (int j = 0; j < 2 && all; j++)
                    for (int i = 0; i < 2; i++) {
                        if (!MOUNTAINOUS(c + i, r + j) ||
                            DONE(c + i, r + j)) { all = 0; break; }
                    }
                if (!all) continue;

                int above = 0, below = 0;
                for (int i = 0; i < 2; i++) {
                    if (r > 0      && MOUNTAINOUS(c + i, r - 1)) above = 1;
                    if (r + 2 < MH && MOUNTAINOUS(c + i, r + 2)) below = 1;
                }
                /* cap at the top of the mass, foot at the bottom, and slope A/B alternating
                 * by block row in between — which is what lines the diamonds into ridges */
                int row = !above ? 0 : (!below ? 3 : (((r >> 1) & 1) ? 2 : 1));
                int row2 = (row == 0) ? 1 : (row == 3 ? 3 : (row == 1 ? 2 : 1));
                MoteSprite a = { &mtn_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                                 0, (uint16_t)(row * TILE), 2 * TILE, TILE, 14, 0 };
                MoteSprite b = { &mtn_img, (int16_t)(c * TILE), (int16_t)((r + 1) * TILE),
                                 0, (uint16_t)(row2 * TILE), 2 * TILE, TILE, 14, 0 };
                add(&a); add(&b);
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++)
                        if (c + i >= c0 - 1 && c + i <= c0 + MVW + 2 &&
                            r + j >= r0 - 1 && r + j <= r0 + MVH + 2) DONE(c + i, r + j) = 1;
            }
        }

        /* PASS 2: every mountain cell the core missed gets a lone crag */
        for (int r = r0; r <= r0 + MVH; r++) {
            if (r < 0 || r >= MH) continue;
            for (int c = c0; c <= c0 + MVW; c++) {
                if (c < 0 || c >= MW) continue;
                if (!MOUNTAINOUS(c, r) || DONE(c, r)) continue;
                int v = ((c * 7 + r * 3) & 1);
                MoteSprite spr = { &mtn_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                                   (uint16_t)(v * TILE), (uint16_t)(4 * TILE),
                                   TILE, TILE, 14, 0 };
                add(&spr);
            }
        }
        #undef DONE
        #undef MOUNTAINOUS
        (void)in_win;
    }

    /* LAVA AND SCORCHED, the band that would not fit. Eight layers hold eight bands and
     * there is no ninth, so the rarest pair moved to sprites — measured at 0.0-0.6% of a
     * world, about one cell in view. They always carry flux and FX anyway. */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t b = mb_w.biome[AT(c, r)];
            if (b != B_LAVA && b != B_SCORCHED) continue;
            int col = (b == B_LAVA) ? 0 : 1;
            int v = ((c * 7 + r * 3) & 1);
            MoteSprite spr = { &hot_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               (uint16_t)(col * TILE), (uint16_t)(v * TILE),
                               TILE, TILE, 13, 0 };
            add(&spr);
        }
    }

    /* (The per-cell transition sprites that used to live here are gone: the band stack
     * draws every boundary automatically, so terrain costs no sprites at all and can
     * never be starved by a passing deer.) */

    /* MOUNTAINS, as COMPOSED SPRITES on a two-cell grid.
     *
     * The artist's mountain is two columns by four rows drawn to fit together — cap,
     * two alternating slope rows that make diamond peaks, and a foot with a snowline.
     * A tileset cannot place it: the engine picks a variant row with a position HASH,
     * so three attempts at shipping his cells as `nvar` variants shuffled a composed
     * picture into confetti. A sprite has no grid, so the phase is chosen HERE, on
     * purpose: snap to even cells, and pick his row from where the block sits in the
     * mass. Slope A on even block rows and slope B on odd is what makes the diamonds
     * line up into ridges instead of repeating one triangle.
     *
     * One sprite per 2x2 block rather than per cell, so a mountainous screen costs
     * about a dozen and not fifty. */
    for (int r = (r0 & ~1); r <= r0 + MVH + 1; r += 2) {
        if (r < 0 || r >= MH) continue;
        for (int c = (c0 & ~1); c <= c0 + MVW + 1; c += 2) {
            if (c < 0 || c >= MW) continue;
            /* does this block contain any mountain at all? */
            int hit = 0;
            for (int j = 0; j < 2 && !hit; j++)
                for (int i = 0; i < 2; i++) {
                    int x = c + i, y = r + j;
                    if (mb_in(x, y) && mb_w.biome[AT(x, y)] == B_MOUNTAIN) { hit = 1; break; }
                }
            if (!hit) continue;

            /* where does it sit in the mass? nothing above -> cap, nothing below -> foot */
            int above = 0, below = 0;
            for (int i = 0; i < 2; i++) {
                if (mb_in(c + i, r - 1) && mb_w.biome[AT(c + i, r - 1)] == B_MOUNTAIN) above = 1;
                if (mb_in(c + i, r + 2) && mb_w.biome[AT(c + i, r + 2)] == B_MOUNTAIN) below = 1;
            }
            int row = !above ? 0 : (!below ? 3 : (((r >> 1) & 1) ? 2 : 1));

            MoteSprite spr = { &mtn_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               0, (uint16_t)(row * TILE), 2 * TILE, TILE, 14, 0 };
            add(&spr);
            /* the block is two cells tall, so the second row of it takes the next
             * strip down — slope alternates, cap is followed by slope, foot by foot */
            int row2 = (row == 0) ? 1 : (row == 3 ? 3 : (row == 1 ? 2 : 1));
            MoteSprite spr2 = { &mtn_img, (int16_t)(c * TILE), (int16_t)((r + 1) * TILE),
                                0, (uint16_t)(row2 * TILE), 2 * TILE, TILE, 14, 0 };
            add(&spr2);
        }
    }

    /* ROADS, over the terrain and under everything else. The cell is chosen from the
     * FOUR-NEIGHBOUR MASK, because a road is a graph and not an area: what matters is
     * only which cardinals it continues into. Sixteen cells cover every case exactly —
     * a stone, four dead ends, two straights, four corners, four tees and a crossroads
     * — where a 47-cell blob set would be answering the wrong question. */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW || !mb_w.road[AT(c, r)]) continue;
            int m = 0;
            if (r > 0      && mb_w.road[AT(c, r - 1)]) m |= 1;
            if (c < MW - 1 && mb_w.road[AT(c + 1, r)]) m |= 2;
            if (r < MH - 1 && mb_w.road[AT(c, r + 1)]) m |= 4;
            if (c > 0      && mb_w.road[AT(c - 1, r)]) m |= 8;
            MoteSprite spr = { &road_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               (uint16_t)(m * TILE), 0, TILE, TILE, 12, 0 };
            add(&spr);
        }
    }

    /* LAST: ground clutter — trees, rocks, tufts, headstones. Whatever the cap
     * eats, it eats here. */
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
            add(&spr);
        }
    }
}
