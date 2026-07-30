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
#include "rocks.h"
#include "blob47_same.h"
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
#include "hot.h"

/* The town sheet's geometry, in one place: 8 wide, 14 tall, drawn six pixels above
 * its own tile. Must match authoring/build_sprites.py. */
#define TOWN_W 8
#define TOWN_H 14
#define TOWN_ANCHOR_Y (-6)
/* The sheet's column IS `obj - O_BUILD0` for the first O_N - O_BUILD0 columns, which is fast
 * and has no lookup table to drift — but it means adding an O_* without adding a column
 * silently draws the wrong building for every id after it. This catches exactly that, at
 * compile time. It is >= rather than == because the sheet also carries the LATER-AGE FORMS
 * (see MB_MODERN below) in columns past the object range. */
typedef char mb_town_covers_builds[(town_W / TOWN_W) >= (O_N - O_BUILD0) ? 1 : -1];
#include "ui_status.h"
#include "tools.h"
#include "crowns_fx.h"
#include "fx_frost.h"
#include "fx_acid.h"
#include "characters.h"
#include "animals.h"
#include "monsters.h"
#include "bosses.h"   /* the seventeen kaiju, 2x2 each */
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
/* BURNT GROUND IS CHARCOAL, not MAROON. Scorched earth was the artist's dark red-purple
 * with red dashes through it, and on screen a burnt village read as a patch of BRIGHT
 * PINK BRICKWORK — the aftermath of a fire looked cheerful. Sixteen palette entries are
 * a constraint on one image, not on the world: the screen is RGB565, so soot can just be
 * soot. Ember is what still glows in it, and it is dull on purpose so the live fire
 * stays the brightest thing on the map. */
#define C_CHAR    MOTE_RGB565( 70,  52,  46)
#define C_ASH     MOTE_RGB565(108, 100,  96)

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
    C_BROWN,   /* tundra   */ C_WHITE,  /* snow     */ C_ASH,    /* ash      */
    C_CHAR,    /* scorched */ C_RED,    /* lava     */ C_GREEN,  /* acid     */
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
static int      s_lut_age = -1, s_lut_tint = -1, s_lut_mode = -1;
static uint32_t s_lut_sig;
/* village id -> LUT ROW, so the border test is four array reads instead of four calls
 * into mb_kingdom_of() with its bounds checks. Those four calls per claimed pixel were
 * the whole remaining cost after the colour table went in. */
static uint8_t  s_kof[MAXV];

/* --- MAP MODES: every loop, on one map ---------------------------------
 *
 * A simulation you cannot see is a simulation you have to be told about. Tech, creed and
 * tier are all facts about a TOWN, which at one pixel per tile is the natural scale for
 * them — so instead of annotating the world with icons (the mistake the status pips made),
 * God's Eye recolours: the same political map, keyed by a different fact.
 *
 * It costs nothing, because the tint was already a lookup table over (owner, biome). Only
 * the KEY and the palette change: the row index per village, and the colour per row. Which
 * is why four map modes came to about thirty lines.
 *
 *   POWER   who holds it     — kingdom colours, borders drawn hard
 *   FAITH   what it believes — one colour per creed, so a schism is a shape on the map
 *   CRAFT   what it is for   — farm/forest/mine/port, so you can read the economy
 *   GROWTH  how far it got   — camp to city, so a run's history is one glance
 */
static int s_mapmode;
int  mb_draw_mapmode(void)          { return s_mapmode; }
void mb_draw_mapmode_set(int m)     { s_mapmode = (m < 0 || m >= MAPMODE_N) ? 0 : m; }
const char *const MB_MAPMODE_NAME[MAPMODE_N] = { "POWER", "FAITH", "CRAFT", "GROWTH" };

/* The palettes. Creeds get four distinct hues; crafts borrow the colour of the thing they
 * work (green wood, grey stone, gold grain, blue water); growth runs dark to bright, so a
 * city is the brightest thing on the map and a camp barely shows. */
static const uint16_t CREED_COL[CREED_N] = {
    0, MOTE_RGB565(255, 214,  60), MOTE_RGB565(150, 170, 255),
       MOTE_RGB565(170, 160, 150), MOTE_RGB565( 60, 210, 200),
};
static const uint16_t CRAFT_COL[PROF_N] = {
    0, MOTE_RGB565(255, 200,  70),   /* farmer   — standing grain */
       MOTE_RGB565( 40, 190,  90),   /* forester — timber         */
       MOTE_RGB565(190, 190, 200),   /* miner    — stone          */
       MOTE_RGB565(255,  90,  90),   /* soldier                   */
       MOTE_RGB565(230, 180, 255),   /* priest                    */
       MOTE_RGB565( 70, 180, 255),   /* trader   — the sea        */
       MOTE_RGB565(255, 150,  60),   /* builder                   */
       MOTE_RGB565(255, 240, 200),   /* lord                      */
};
static const uint16_t GROWTH_COL[TIER_N] = {
    MOTE_RGB565( 70,  70,  90), MOTE_RGB565(120, 110, 110),
    MOTE_RGB565(180, 160, 120), MOTE_RGB565(240, 210, 120),
    MOTE_RGB565(255, 250, 220),
};

/* The lens colour for one settlement, which the tint table and the town markers both
 * need — so the two can never disagree about what green means. */
static uint16_t lens_col(int v)
{
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) return 0;
    switch (s_mapmode) {
    case MAPMODE_FAITH:  return CREED_COL[mb_v[v].creed < CREED_N ? mb_v[v].creed : 0];
    case MAPMODE_CRAFT:  return CRAFT_COL[mb_v[v].spec  < PROF_N  ? mb_v[v].spec  : 0];
    case MAPMODE_GROWTH: return GROWTH_COL[mb_v[v].tier < TIER_N  ? mb_v[v].tier  : 0];
    default:             return mb_kingdom_colour(mb_v[v].kingdom);
    }
}

void mb_draw_prepare(void)
{
    int amt; uint16_t age_col = mb_age_tint(&amt);
    int tint = mb_law(LAW_TINT);
    int mode = s_mapmode;
    /* a signature over what the table depends on, so it rebuilds only on a change */
    uint32_t sig = (uint32_t)mb_age_id() * 131u + (uint32_t)tint * 7u + (uint32_t)mode * 977u;
    for (int k = 1; k < MAXK; k++)
        sig = sig * 31u + (uint32_t)(mb_k[k].alive ? (mb_k[k].colour + 1) : 0);

    /* THE KEY PER VILLAGE, which is the whole of what a map mode is. */
    for (int v = 0; v < MAXV; v++) {
        int key = 0;
        if (v > 0 && mb_v[v].alive) {
            switch (mode) {
            case MAPMODE_FAITH:  key = mb_v[v].creed; break;
            case MAPMODE_CRAFT:  key = mb_v[v].spec;  break;
            case MAPMODE_GROWTH: key = mb_v[v].tier + 1; break;   /* +1: 0 means unclaimed */
            default:             key = mb_v[v].kingdom; break;
            }
        }
        if (key > MAXK) key = MAXK;
        s_kof[v] = (uint8_t)key;
        sig = sig * 17u + (uint32_t)key;
    }
    if (sig == s_lut_sig && s_lut_age == mb_age_id()
        && s_lut_tint == tint && s_lut_mode == mode) return;
    s_lut_sig = sig; s_lut_age = mb_age_id(); s_lut_tint = tint; s_lut_mode = mode;

    for (int k = 0; k <= MAXK; k++) {
        uint16_t kc;
        switch (mode) {
        case MAPMODE_FAITH:  kc = (k > 0 && k < CREED_N) ? CREED_COL[k] : 0; break;
        case MAPMODE_CRAFT:  kc = (k > 0 && k < PROF_N)  ? CRAFT_COL[k] : 0; break;
        case MAPMODE_GROWTH: kc = (k > 0 && k <= TIER_N) ? GROWTH_COL[k - 1] : 0; break;
        default:             kc = (k > 0) ? mb_kingdom_colour(k) : 0; break;
        }
        /* The non-political modes wash HARDER, because they are the point of the view
         * while they are on: at the political tint's 40/255 a creed was a suggestion. */
        int wash_in = (mode == MAPMODE_POWER) ? 40 : 120;
        for (int b = 0; b < B_COUNT; b++) {
            uint16_t base = MB_COL[b];
            uint16_t in = base, ed = base;
            if ((tint || mode != MAPMODE_POWER) && kc) {
                in = blend565(base, kc, wash_in);
                ed = blend565(base, kc, 220);
            }
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

/* SNOWLINE: above this the rock goes white, and because it follows the elevation field
 * the caps land on the actual summits and follow their shape. This is the one thing the
 * icons did give for free, and it is cheaper to keep it here than to draw it. */
#define RL_SNOW 222

static int rl_hard(uint8_t b);          /* see SHADED RELIEF below */

/* The engine picks an autotile variant from a position hash so a big area does not
 * repeat; these patches are drawn by hand as sprites, so they need the same trick. */
static inline unsigned mb__hash2(int x, int y)
{
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u;
    h ^= h >> 13; return h * 1274126177u;
}

/* The camera the deferred sprite positions — and the sea's scroll — are against. */
static int s_cam_px, s_cam_py;

/* --- THE DEEP, as a background pass -----------------------------------------
 *
 * Deep ocean is the only terrain with no tileset: it is the scene BACKGROUND, which the
 * engine fills with one flat colour before the band stack draws over it. That is why it
 * paid nothing — and why it was a slab of plain navy with a textured coast sitting on it.
 * A background CALLBACK costs the same as the fill it replaces, so the deep gets a real
 * surface for nothing.
 *
 * IT IS THE SHALLOW BAND'S PATTERN, DARKER. That is the whole specification and it is the
 * right one: the deep and the shoals are the same sea, so they should be the same water
 * with different light in it, and the ripple dashes bands.ripples() lays into the shallow
 * tileset are already what a wave crest looks like from above. Two earlier attempts went
 * looking for something cleverer and both were worse — crossing swells interfere into a
 * regular diamond quilt, and swell BANDING over a handful of tones draws wide diagonal
 * stripes whose edges are more visible than the swell. Neither was a thing water does.
 *
 * The lattice is the same 8 px pitch as a tile and the dashes are placed by a POSITION
 * HASH, so it has no period, and it does not animate — nothing else in Mortal View does,
 * and a surface sliding under a fixed coastline reads as the LAND moving.
 */
#define SEA_GRID   8          /* the lattice, one cell per tile, as in the tileset */
#define SEA_DASHES 3          /* per cell — bands.ripples() lays three, so this lays three */
static const uint16_t SEA_BODY  = MOTE_RGB565( 29,  43,  83);   /* C_NAVY: MB_COL[B_OCEAN] */
static const uint16_t SEA_CREST[2] = {
    MOTE_RGB565( 45,  63, 112),   /* the ripple */
    MOTE_RGB565( 62,  84, 140),   /* and the odd brighter one, as in the shallows */
};

void mb_draw_sea_band(uint16_t *fb, int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
    const int cx = s_cam_px, cy = s_cam_py;

    for (int y = y0; y < y1; y++) {
        uint16_t c = (y >= VIEW_H) ? MOTE_RGB565(12, 14, 26)   /* HUD strip: overlay's */
                                   : SEA_BODY;
        for (int x = 0; x < MW; x++) px_put(fb, x, y, c);
    }

    if (y0 >= VIEW_H) return;
    const int ylim = y1 < VIEW_H ? y1 : VIEW_H;
    /* One extra cell of margin each way: a dash starting just off screen still has to
     * draw the part of itself that is on it. */
    const int gy0 = (y0 + cy) / SEA_GRID - 1, gy1 = (ylim + cy) / SEA_GRID + 1;
    const int gx0 = cx / SEA_GRID - 1,        gx1 = (MW + cx) / SEA_GRID + 1;
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            unsigned h = mb__hash2(gx, gy);
            for (int d = 0; d < SEA_DASHES; d++) {
                h = h * 1103515245u + 12345u;
                int sy = gy * SEA_GRID + (int)((h >> 6) & 7) - cy;
                if (sy < y0 || sy >= ylim) continue;
                int sx  = gx * SEA_GRID + (int)((h >> 3) & 7) - cx;
                int len = 2 + (int)((h >> 12) & 2);
                uint16_t col = SEA_CREST[(h >> 14) & 3 ? 0 : 1];
                for (int k = 0; k < len; k++) {
                    int xx = sx + k;
                    if (xx >= 0 && xx < MW) px_put(fb, xx, sy, col);
                }
            }
        }
    }
}

/* --- TOWN MARKERS: a settlement is a PLACE, not a few white pixels -------
 *
 * At one pixel per tile a village is a smudge of white building dots, and the first attempt
 * at map lenses proved why that is not enough: recolouring the CLAIM changed almost nothing
 * on screen, because claimed ground is a small fraction of a world and the interior wash is
 * deliberately light. Four lenses looked like four copies of the same map.
 *
 * So every living settlement gets a mark whose SIZE is its tier and whose COLOUR is the
 * lens. That reads at a glance in every mode — where the towns are, which faith holds them,
 * what they make, how far they have come — and it fixes the political map too, where a
 * capital was previously indistinguishable from a hamlet.
 */
void mb_god_towns(uint16_t *fb, int y0, int y1)
{
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive) continue;
        uint16_t c = lens_col(v);
        if (!c) c = C_WHITE;
        int tier = mb_v[v].tier < TIER_N ? mb_v[v].tier : 0;
        int r = (tier >= TIER_CITY) ? 2 : (tier >= TIER_TOWN) ? 2 : (tier >= TIER_VILLAGE) ? 1 : 1;
        int cx = mb_v[v].x, cy = mb_v[v].y;
        /* a dark surround first, so the mark reads on snow and on desert alike — the same
         * two-tone trick the cursor needs for the same reason */
        for (int dy = -r - 1; dy <= r + 1; dy++)
            for (int dx = -r - 1; dx <= r + 1; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || x >= MW || y < y0 || y >= y1 || y >= VIEW_H) continue;
                int inner = (dx >= -r && dx <= r && dy >= -r && dy <= r);
                if (!inner) px_put(fb, x, y, C_NAVY);
            }
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || x >= MW || y < y0 || y >= y1 || y >= VIEW_H) continue;
                px_put(fb, x, y, c);
            }
        /* a CITY carries a bright core, so the great places stand out from the good ones */
        if (tier >= TIER_CITY && cy >= y0 && cy < y1 && cy < VIEW_H)
            px_put(fb, cx, cy, C_WHITE);
    }
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

            /* SHADED RELIEF, and THIS is the view it belongs in. One pixel a tile puts
             * the whole world on screen, so a mountain range is visible as a range: the
             * hillshade draws its ridges, spurs, cols and valleys straight out of the
             * elevation field, and the snowline lands on the actual summits. Six attempts
             * at mountain ART all failed because a close-up sees sixteen tiles of a
             * sixty-tile massif and there is no shape there to draw. Here there is.
             *
             * Restricted to rock so the cost is the 19% of cells that are rock (measured)
             * rather than every pixel — this runs per frame, and the colour table above
             * exists precisely because per-pixel arithmetic here was too expensive. */
            if (b >= 1 && !o && rl_hard(b) && x > 0 && x < MW - 1 && y > 0 && y < MH - 1) {
                const uint8_t *e = mb_w.elev + y * MW + x;
                int lit = -((int)e[1] - (int)e[-1]) - ((int)e[MW] - (int)e[-MW]);
                int R = (c >> 11) & 31, G = (c >> 5) & 63, B = c & 31;
                if (lit > 9)  lit = 9;
                if (lit < -9) lit = -9;
                if (lit > 0)      { R += ((31 - R) * lit * 12) >> 7;
                                    G += ((63 - G) * lit * 12) >> 7;
                                    B += ((31 - B) * lit * 12) >> 7; }
                else if (lit < 0) { int k = 128 + lit * 10;
                                    R = (R * k) >> 7; G = (G * k) >> 7; B = (B * k) >> 7; }
                int sn = (*e > RL_SNOW) ? (*e - RL_SNOW) * 15 : 0;
                if (sn > 205) sn = 205;
                if (sn) { R += ((31 - R) * sn) >> 8;
                          G += ((63 - G) * sn) >> 8;
                          B += ((31 - B) * sn) >> 8; }
                c = (uint16_t)((R << 11) | (G << 5) | B);
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
    { &rocks_img,         0, 0 },       /* rock      — ours; see authoring/rocks.py */
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
    /* ROCK IS OURS NOW. The master's boulders sheet holds FRAGMENTS OF A COMPOSED
     * MOUNTAIN, not loose stones: the label set calls (4,1) "mountain slope", and
     * scattering it over peaks drew 59 identical pale right-triangles in one 16x14 view
     * — counted with MOTEBOX_CENSUS, and the reason six rounds of mountains looked like
     * confetti. (0,1), its "grey boulder", is a flat khaki square. Both are cells of a
     * picture that only works in phase with its neighbours, which an object never is. */
    { &rocks_img,         1, 0 },       /* boulder   — an outcrop breaking the surface */
    { &rocks_img,         2, 0 },       /* crag      — a snow-crowned high crag        */
    { &nature_img,       15, 0 },       /* grave     "tombstone"  (was a GHOST)  */
};
/* The tripwire O_NAME has. This array was 15 entries long while the enum grew past
 * 30, so graves drew nothing at all: a battlefield with thirty-four dead showed bare
 * grass. A short array is silent, which is exactly why it needs a check. */
typedef char mb_objspr_covers_world[(sizeof MB_OBJ_SPR / sizeof MB_OBJ_SPR[0]) >= O_BUILD0 ? 1 : -1];

/* --- THE CAST: who plays each part ---------------------------------------
 *
 * One figure per race and five profession cells SHARED by all of them is what this replaces.
 * Nine cells of ninety were in use, an elf soldier looked human, and a crowd of twenty humans
 * was twenty identical sprites. Chosen by hand in the casting tool rather than by me guessing
 * from coordinates, which is the only way this ever went right: two of my own picks — the
 * "miner" at 10,3 and the "trader" at 12,3 — turned out to be unreadable blobs at 8x8.
 *
 * THREE THINGS COME OUT OF IT and none needs a legend:
 *   a trade is legible from the figure — mail, cowl, helm, hard hat, travelling cloak
 *   a race stays a race, because each has its own cells for the same trades
 *   AGE SHOWS. A child is drawn as a child and an idle elder as an elder, so a village reads
 *   as a village with generations in it rather than as a squad of identical adults.
 *
 * Ragged on purpose: elves have no builder or trader cell and fall back to their civilian
 * pool, which is exactly what the fallback chain is for.
 */
enum { CR_CIVILIAN = 0, CR_CHILD, CR_ELDER, CR_FARMER, CR_BUILDER, CR_TRADER,
       CR_FORESTER, CR_MINER, CR_SOLDIER, CR_PRIEST, CR_LORD, CR_N };
#define CAST_MAX 3
#define CH 0            /* the characters section */
#define MO 1            /* the monsters section  */
typedef struct { uint8_t sh, cx, cy; } Fig;
typedef struct { uint8_t n; Fig v[CAST_MAX]; } CastSlot;

#define F0                        { 0, { { 0, 0, 0 } } }
#define F1(s,x,y)                 { 1, { { s, x, y } } }
#define F2(s,x,y,t,u,w)           { 2, { { s, x, y }, { t, u, w } } }
#define F3(s,x,y,t,u,w,a,b,c)     { 3, { { s, x, y }, { t, u, w }, { a, b, c } } }

static const CastSlot MB_CAST[SP_CIV_N][CR_N] = {
    /* ---- human ---- */
    { F2(CH,4,1, CH,6,0),      /* civilian */
      F2(CH,0,3, CH,1,3),      /* child    */
      F1(CH,3,3),              /* elder    */
      F1(CH,4,3),              /* farmer   */
      F1(CH,7,1),              /* builder  */
      F2(CH,5,1, CH,8,6),      /* trader   */
      F1(CH,0,1),              /* forester */
      F1(CH,6,1),              /* miner    */
      F2(CH,3,1, CH,4,0),      /* soldier  */
      F1(CH,8,0),              /* priest   */
      F2(CH,2,1, CH,9,6) },    /* lord     */
    /* ---- elf ---- */
    { F1(CH,8,1), F1(CH,14,0), F1(CH,7,0), F1(CH,9,0),
      F0,                      /* no builder: falls back to the civilian pool */
      F0,                      /* no trader either */
      F1(CH,8,1), F1(CH,10,0), F1(CH,5,0), F1(CH,1,1), F1(CH,15,0) },
    /* ---- dwarf ---- */
    { F1(CH,5,3), F1(CH,6,3), F1(CH,3,3), F1(CH,4,3), F1(CH,5,3), F1(CH,4,3),
      F1(CH,7,3), F1(CH,8,3), F1(CH,9,3), F1(CH,7,3), F1(CH,3,5) },
    /* ---- orc: its own figure on the monsters section, its trades on characters ---- */
    { F1(MO,7,4), F1(CH,11,3), F1(CH,12,3), F1(CH,2,4), F1(CH,3,4), F1(CH,2,4),
      F1(CH,2,4), F3(CH,4,4, CH,3,4, CH,0,5), F3(CH,5,4, CH,13,5, CH,5,5),
      F1(CH,6,4), F1(CH,7,4) },
};
/* A short array is silent, and this game has been bitten by exactly that twice. */
typedef char mb_cast_is_complete[(sizeof MB_CAST / sizeof MB_CAST[0]) == SP_CIV_N ? 1 : -1];

/* PROF_* -> CR_*. Kept as a table rather than a switch so adding a profession is one line
 * in two places instead of a hunt through the draw pass. */
static const uint8_t MB_PROF_ROLE[PROF_N] = {
    CR_CIVILIAN, CR_FARMER, CR_FORESTER, CR_MINER, CR_SOLDIER,
    CR_PRIEST, CR_TRADER, CR_BUILDER, CR_LORD,
};

/* The figure for one person: age first, then trade, then the pool, then the species cell.
 * `seed` is the unit's own — a stable pick, so somebody does not change face as they walk. */
static void mb_cast_pick(const Unit *u, unsigned seed, int *sheet, int *cx, int *cy)
{
    const MbSpecies *S = &MB_SP[u->sp];
    *sheet = S->sheet; *cx = S->cx; *cy = S->cy;
    if (u->sp >= SP_CIV_N) return;                  /* beasts and the risen keep their own */

    int span = S->lifespan ? S->lifespan : 20;
    int prof = u->prof < PROF_N ? u->prof : PROF_NONE;
    int role = MB_PROF_ROLE[prof];
    /* A CHILD IS A CHILD whatever trade it was born into; an ELDER shows their age only
     * once they have no trade left to show, so a working smith stays a smith to the end. */
    if (u->age * 5 < span)                       role = CR_CHILD;
    else if (role == CR_CIVILIAN && u->age * 4 > span * 3) role = CR_ELDER;

    const CastSlot *slot = &MB_CAST[u->sp][role];
    if (!slot->n) slot = &MB_CAST[u->sp][CR_CIVILIAN];
    if (!slot->n) return;
    const Fig *f = &slot->v[seed % slot->n];
    *sheet = f->sh ? 1 : 0; *cx = f->cx; *cy = f->cy;
}

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
/* --- LATER-AGE FORMS ----------------------------------------------------
 *
 * A house is a house for the whole of history, which is wrong twice: a city that has reached
 * the industrial era should not be built out of the same thatched cottages it started with,
 * and the tech tree should show in the streets and not only in a menu.
 *
 * These are not new objects. They are extra COLUMNS on the town sheet, chosen at draw time
 * from the tech of the kingdom that owns the cell — the same trick the road surfaces use, and
 * for the same reasons: it costs no storage per cell, it needs no want list or build cost, and
 * it is RETROACTIVE. The moment a realm learns steam, every house it owns is a tenement; if
 * that realm falls and the ground is taken by a stone-age neighbour, they are cottages again.
 *
 * The column indices are past O_N - O_BUILD0, which is why the tripwire above is >= and not ==.
 */
#define TOWN_COL_APARTMENT (O_N - O_BUILD0 + 0)
#define TOWN_COL_TOWER     (O_N - O_BUILD0 + 1)
#define TOWN_COL_CITYBLOCK (O_N - O_BUILD0 + 2)
/* --- ALTERNATIVE DESIGNS ---------------------------------------------------
 * A street of identical sprites reads as wallpaper however good the sprite is, and the banner
 * rows only vary the COLOUR — vary that alone and a street looks like a paint chart. These
 * differ in silhouette: roof pitch, gable direction, storey count, a lean-to, a shopfront.
 * One is chosen per cell by a position hash, so it is stable (a house does not flicker between
 * designs) and neighbours rarely match. */
#define TOWN_COL_HOUSE_B   (O_N - O_BUILD0 + 3)
#define TOWN_COL_HOUSE_C   (O_N - O_BUILD0 + 4)
#define TOWN_COL_COTTAGE_B (O_N - O_BUILD0 + 5)
#define TOWN_COL_MANOR_B   (O_N - O_BUILD0 + 6)
#define TOWN_COL_APT_B     (O_N - O_BUILD0 + 7)
#define TOWN_COL_APT_C     (O_N - O_BUILD0 + 8)
/* and what the founding campfire becomes: fire, then water, then light */
#define TOWN_COL_WELL      (O_N - O_BUILD0 + 9)
#define TOWN_COL_GASLAMP   (O_N - O_BUILD0 + 10)
#define TOWN_COL_LIGHT     (O_N - O_BUILD0 + 11)

/* Alternatives per column, including the base as the first entry. Indexed by the column that
 * has already been resolved for era, so a modern block draws from the modern set. */
typedef struct { uint8_t n, col[3]; } VariantSet;
static const VariantSet MB_VARIANT[] = {
    /* the plain house, the gable-end and the narrow two-storey */
    { 3, { O_HOUSE1 - O_BUILD0, TOWN_COL_HOUSE_B, TOWN_COL_HOUSE_C } },
    { 2, { O_HOUSE2 - O_BUILD0, TOWN_COL_COTTAGE_B, 0 } },
    { 2, { O_HOUSE3 - O_BUILD0, TOWN_COL_MANOR_B, 0 } },
    { 3, { TOWN_COL_APARTMENT,  TOWN_COL_APT_B, TOWN_COL_APT_C } },
};
#define MB_NVARIANT ((int)(sizeof MB_VARIANT / sizeof MB_VARIANT[0]))

/* A LADDER, not one swap. The campfire needs three rungs — a settlement is not still
 * gathering round an open fire once it has a power station, and going straight from fire to
 * electric light skips the whole of history in between. Rungs are listed OLDEST FIRST and the
 * last one whose tech is known wins, so a kingdom shows the most recent thing it can build. */
typedef struct { uint8_t col, tech; } Rung;
typedef struct { Rung up[3]; } ModernForm;
static const ModernForm MB_MODERN[O_N - O_BUILD0] = {
    /* the founding fire: a fire, then a well, then a gas lamp, then a street light —
     * the same idea each time, the thing the town gathers round */
    [O_FIRE_PIT - O_BUILD0] = {{ { TOWN_COL_WELL,    TECH_MASONRY  },
                                 { TOWN_COL_GASLAMP, TECH_STEAM    },
                                 { TOWN_COL_LIGHT,   TECH_ELECTRIC } }},
    [O_HOUSE1 - O_BUILD0] = {{ { TOWN_COL_APARTMENT, TECH_STEAM } }},
    [O_HOUSE2 - O_BUILD0] = {{ { TOWN_COL_APARTMENT, TECH_STEAM } }},
    [O_HOUSE3 - O_BUILD0] = {{ { TOWN_COL_TOWER,     TECH_STEAM } }},
    [O_MARKET - O_BUILD0] = {{ { TOWN_COL_CITYBLOCK, TECH_ELECTRIC } }},
};

/* WHICH COLUMN A BUILDING ACTUALLY DRAWS AS, given who owns it and where it stands: the
 * era form first, then an alternative design for that form. Split out of the draw loop so
 * the audit can tally it (MOTEBOX_LOOPS) — "does the campfire become a street light" is a
 * question about one kingdom's tech and one cell's hash, and counting is the only honest way
 * to answer it. Squinting at a screenshot of a town the camera happened to stop on is not. */
int mb_draw_form_col(uint8_t o, int k, int c, int r)
{
    int col = o - O_BUILD0;
    const ModernForm *mf = &MB_MODERN[col];
    for (int u = 0; u < 3; u++)
        if (mf->up[u].tech && mb_tech_known(k, mf->up[u].tech)) col = mf->up[u].col;
    for (int vi = 0; vi < MB_NVARIANT; vi++)
        if (MB_VARIANT[vi].col[0] == col)
            return MB_VARIANT[vi].col[mb__hash2(c, r) % MB_VARIANT[vi].n];
    return col;
}

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
 * refused. The list fills at MB_MAXSPR and a full list drops silently, and "the buildings
 * are missing" was exactly that going unnoticed — so it is counted, and MOTEBOX_PERF=1
 * says SCENE FULL out loud rather than leaving the next person to census the map. A
 * mature city measured 232 wanted, which is why the cap is not 224. */
static int s_spr_want, s_spr_lost;
void mb_draw_sprite_load(int *want, int *lost) { *want = s_spr_want; *lost = s_spr_lost; }

/* THE SPRITES ARE HELD BACK, and drawn by us after the shaded relief.
 *
 * The relief reads the framebuffer and writes it back, so whatever is already there gets
 * shaded — and the engine rasters the tilemap and then its sprites in one call, with no
 * hook in between. Handing the sprites to the scene therefore lit a deer from the
 * north-west along with the hillside it was standing on, and put a snow cap on a house.
 *
 * The alternative was to exclude each sprite's BOX from the relief, and that is worse: a
 * tree's box is a whole tile, so a shaded slope would get an unshaded square wherever
 * anything stood on it — the same "a creature makes its tile look different" artefact
 * this game has already been caught with once.
 *
 * So they are collected here and blitted in mb_draw_mortal_sprites(), in layer order,
 * after the ground is finished. Same immediate-mode blit the engine would have used, same
 * order, and the relief cannot reach them. It also puts the sprite budget in our hands
 * rather than the scene's shared cap.
 */
#define MB_MAXSPR 320
static MoteSprite s_defer[MB_MAXSPR];
static int        s_ndefer;

static void add(const MoteSprite *s)
{
    s_spr_want++;
    if (s_ndefer >= MB_MAXSPR) { s_spr_lost++; return; }
    s_defer[s_ndefer++] = *s;
}

/* Layer order, lower first — the engine's own rule, so nothing about the draw order
 * changes by moving the blit over here. */
void mb_draw_mortal_sprites(uint16_t *fb)
{
    int maxl = 0;
    for (int i = 0; i < s_ndefer; i++) if (s_defer[i].layer > maxl) maxl = s_defer[i].layer;
    for (int pass = 0; pass <= maxl; pass++)
        for (int i = 0; i < s_ndefer; i++) {
            const MoteSprite *p = &s_defer[i];
            if (p->layer != pass) continue;
            g_api->blit(fb, p->img, p->x - s_cam_px, p->y - s_cam_py,
                        p->fx, p->fy, p->fw, p->fh, p->flags, 0, VIEW_H);
        }
}

/* --- SHADED RELIEF: mountains are HIGH GROUND WITH LIGHT ON IT ------------
 *
 * Six attempts drew mountains as pictures OF mountains — the artist's composed 2x2 block
 * placed four different ways, then triangles of my own — and every one was a little icon
 * stamped on a map. The model was wrong, not the drawing. Seen from directly above, a
 * mountain has no silhouette to draw: what tells you a range is a range is SHADING.
 *
 * So there is no mountain art at all. This shades the ground from the elevation field the
 * world generator already made, which is a smooth 5-octave fbm — a real landform, better
 * than anything I would hand-place. A ridge gets a lit western face and a dark eastern
 * one, a valley reads as a valley, a col reads as a pass, and it works at any size of
 * massif because it is derived from the shape rather than tiled over it.
 *
 * TWO THINGS MAKE OR BREAK IT, both of which the first version got wrong.
 *
 * SMOOTHNESS. Elevation is one byte per tile, so shading a pixel from its own tile's
 * slope paints in 8x8 blocks and the range comes out as a mosaic. The slope is therefore
 * measured once per tile and then interpolated BILINEARLY between tile centres, so the
 * light varies continuously across a face. Cost is one gradient per tile — 224 for a
 * whole screen — and the interpolation is three shifts per pixel.
 *
 * GAIN. Peaks live in elev 214..255, a span of forty, and a tile-to-tile step inside a
 * massif is only a few units. The first version divided by 26 and clamped, so almost
 * everything fell inside its own dead zone and the mountains stayed flat grey. The gain
 * is now per-terrain: rock is shaded hard because that IS the mountain, soft ground is
 * shaded gently so hills and dunes get form without the fields looking crumpled, and
 * water is left alone because a lake surface is level whatever the bed does.
 */

/* Shade per tile in 1/64ths, -64..64, for the window plus a one-tile margin on each side
 * so the interpolation has neighbours at the screen edge. */
#define RL_W (MVW + 3)
#define RL_H (MVH + 3)
static int8_t  s_shade[RL_W * RL_H];
static uint8_t s_snow [RL_W * RL_H];


static int rl_soft(uint8_t b)      /* gently shaded: it has form but it is not rock */
{
    return b == B_GRASS || b == B_MEADOW || b == B_FOREST || b == B_SAVANNA ||
           b == B_DESERT || b == B_BEACH || b == B_SWAMP  || b == B_TUNDRA  ||
           b == B_SNOW   || b == B_FARM;
}

static int rl_hard(uint8_t b)      /* rock: shaded hard, because this is the mountain */
{
    return b == B_HILL || b == B_MOUNTAIN || b == B_PEAK ||
           b == B_RUBBLE || b == B_ASH;
}

void mb_draw_relief(uint16_t *fb, int cam_x, int cam_y)
{
    const int c0 = (cam_x >> 3) - 1, r0 = (cam_y >> 3) - 1;

    /* 1. one slope per tile. The baseline is two tiles wide, not one, because a one-tile
     *    difference on a byte field is mostly quantisation noise. */
    for (int j = 0; j < RL_H; j++) {
        int r = r0 + j;
        for (int i = 0; i < RL_W; i++) {
            int c = c0 + i;
            int8_t *out = &s_shade[j * RL_W + i];
            if (c < 1 || c >= MW - 1 || r < 1 || r >= MH - 1) { *out = 0; continue; }
            uint8_t b = mb_w.biome[AT(c, r)];
            /* Gains are MEASURED, not guessed. Over six worlds the p95 of the lit sum
             * is 9 on rock and 13 on soft ground, so these map that to 55/64 and 26/64:
             * rock gets a face you can read, fields get form without looking crumpled.
             * The first version used 26 and 9, which put nearly the whole world inside
             * the dead zone below — hence six screenshots of flat grey mountains. */
            int gain = rl_hard(b) ? 96 : (rl_soft(b) ? 32 : 0);
            if (!gain) { *out = 0; s_snow[j * RL_W + i] = 0; continue; }   /* water, lava */
            {   /* SNOW, on the same interpolated lattice as the shade. Painting it per
                 * cell instead made a staircase of rectangles across every summit — the
                 * one thing this whole approach exists to avoid. */
                int e = mb_w.elev[AT(c, r)];
                int k = rl_hard(b) && e > RL_SNOW ? (e - RL_SNOW) * 15 : 0;
                s_snow[j * RL_W + i] = (uint8_t)(k > 205 ? 205 : k);
            }
            int dzdx = (int)mb_w.elev[AT(c + 1, r)] - (int)mb_w.elev[AT(c - 1, r)];
            int dzdy = (int)mb_w.elev[AT(c, r + 1)] - (int)mb_w.elev[AT(c, r - 1)];
            int lit = ((-dzdx - dzdy) * gain) >> 4;    /* light from the north-west */
            if (lit >  64) lit =  64;
            if (lit < -64) lit = -64;
            *out = (int8_t)lit;
        }
    }

    /* 2. paint, interpolating between tile CENTRES — a tile centre sits at c*8+4, so the
     *    lattice is offset half a tile from the tile grid and no seam can land on it. */
    for (int sy = 0; sy < VIEW_H; sy++) {
        int v = sy + cam_y - 4;
        int rj = (v >> 3) - r0, fy = v & 7;
        if (rj < 0 || rj + 1 >= RL_H) continue;
        for (int sx = 0; sx < 128; sx++) {
            int u = sx + cam_x - 4;
            int ci = (u >> 3) - c0, fx = u & 7;
            if (ci < 0 || ci + 1 >= RL_W) continue;

            const int8_t *q = &s_shade[rj * RL_W + ci];
            const uint8_t *w = &s_snow[rj * RL_W + ci];
            int a = q[0], bb = q[1], cc = q[RL_W], dd = q[RL_W + 1];
            int sa = w[0], sb = w[1], sc = w[RL_W], sd = w[RL_W + 1];
            if (!(a | bb | cc | dd | sa | sb | sc | sd)) continue;   /* flat bare ground */
            int top = a * (8 - fx) + bb * fx;
            int bot = cc * (8 - fx) + dd * fx;
            int lit = (top * (8 - fy) + bot * fy) >> 6; /* still 1/64ths */
            int stop = sa * (8 - fx) + sb * fx;
            int sbot = sc * (8 - fx) + sd * fx;
            int snow = (stop * (8 - fy) + sbot * fy) >> 6;
            if (lit > -3 && lit < 3 && snow < 4) continue;

            uint16_t p = fb[sy * MOTE_FB_PW + sx];
            int R = (p >> 11) & 31, G = (p >> 5) & 63, B = p & 31;
            if (lit > 0) {                              /* facing the light */
                R += ((31 - R) * lit) >> 7;
                G += ((63 - G) * lit) >> 7;
                B += ((31 - B) * lit) >> 7;
            } else if (lit < 0) {                       /* in its own shadow */
                int k = 128 + lit;                      /* lit is negative here */
                R = (R * k) >> 7; G = (G * k) >> 7; B = (B * k) >> 7;
            }
            if (snow > 0) {                             /* the cap, over the lit rock */
                R += ((31 - R) * snow) >> 8;
                G += ((63 - G) * snow) >> 8;
                B += ((31 - B) * snow) >> 8;
            }
            fb[sy * MOTE_FB_PW + sx] = (uint16_t)((R << 11) | (G << 5) | B);
        }
    }

    /* NO CLIFF LINES. The attempt after this one drew the elevation STEP between
     * neighbouring cells as a dark lip, on the theory that hard edges are what smooth
     * shading lacks. They are — but a step between two cells lies on the tile grid, so
     * every lip was axis-aligned and the mountain came out as a grey wireframe maze. The
     * close-up's texture belongs in the tileset, where it can wander off the grid:
     * bands.stone() breaks the rock surface into angular facets, and the outcrops in
     * authoring/rocks.py stand on the steep ground. Shading here, surface there.
     *
     * The RANGE reads as a range in God's Eye, where the whole world is on screen at one
     * pixel a tile and mb_god_band() shades every rock cell. That is the view a massif
     * fits in; a sixteen-tile window of a sixty-tile massif has no shape to show, and six
     * rounds of trying to draw one there is what proved it.
     */
}

void mb_draw_mortal(int cam_x, int cam_y)
{
    s_spr_want = s_spr_lost = 0;
    s_ndefer = 0;
    g_api->scene2d_begin(cam_x, cam_y);
    s_cam_px = cam_x; s_cam_py = cam_y;
    /* THE WHOLE TERRAIN, from the ruleset system. Deep ocean is the background colour, so
     * a cell with no bits set simply shows through — which is why the stack needs only
     * eight layers where there used to be twenty-three opaque tilesets AND a sprite per
     * boundary cell. */
    g_api->scene_set_background(MB_COL[B_OCEAN - 1]);
    g_api->scene2d_set_autotile_layers(mb_w.layer, MW, MH, MB_BANDS, 8);

    /* DRAW ORDER IS PRIORITY ORDER, not height order.
     *
     * The deferred list holds MB_MAXSPR and fills SILENTLY past that. A 16x14 window is
     * 224 tiles and a mature city measures 232 sprites wanted, so a busy screen can
     * always ask for more than the budget — and with ground clutter going in first, a settled village
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
            int k = mb_kingdom_of(mb_w.claim[AT(c, r)]);
            int col = mb_draw_form_col(o, k, c, r);
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
        int sheet, cx, cy;
        mb_cast_pick(u, mb__hash2(i, u->given), &sheet, &cx, &cy);
        const MoteImage *img = (sheet == 0) ? &characters_img
                             : (sheet == 1) ? &monsters_img : &animals_img;
        MoteSprite spr = { img, (int16_t)(u->x >> 4 << 3), (int16_t)(u->y >> 4 << 3),
                           (uint16_t)(cx * TILE), (uint16_t)(cy * TILE),
                           TILE, TILE, 40, 0 };
        add(&spr);

        /* NO STATUS PIPS. A coloured dot over each villager's head said what they were
         * doing — carrying, foraging, courting, ill, fighting, fleeing, dousing — on the
         * theory that a crowd of identical figures reads as wallpaper. It does, but a
         * legend you have to learn is not the fix: on screen it was thirty coloured
         * squares floating over a town, which is noise, and it made the settlement look
         * LESS like a place. Life has to come from the simulation having more in it, not
         * from annotating what it already has. */
    }
    /* THE WALKING DISASTERS, and the kaiju are 2x2.
     *
     * Every agent used to be drawn as one 8x8 cell of the master's crowns/FX band: the
     * smoke swirl for a tornado and the cone for a vent, and THE SAME TWO for all seven
     * kaiju. So summoning a five-hundred-faith Skull Titan put a little puff of smoke on
     * the map, and the entire bosses sheet — seventeen creatures the extractor already
     * bakes — was never drawn once. The sweep caught it: "a titan walks" in the chronicle
     * with nothing on screen to walk.
     *
     * They are sixteen pixels square, on a two-cell grid whose blocks start at EVEN columns
     * and ODD rows (checked against the sheet, not assumed), so each entry below is a
     * block's top-left cell. Anchored half a tile left and a whole tile up, which puts a
     * two-tile creature's feet on the tile it occupies. */
    for (int i = 0, n = mb_agent_max(); i < n; i++) {
        int ax, ay, kind;
        if (!mb_agent_get(i, &ax, &ay, &kind)) continue;
        typedef struct { const MoteImage *img; uint8_t cx, cy, w; } AgSpr;
        static const AgSpr AG_SPR[AG_N] = {
            [AG_TORNADO] = { &crowns_fx_img, 3, 7, 1 },   /* the smoke swirl */
            [AG_VENT]    = { &crowns_fx_img, 5, 5, 1 },   /* the cone        */
            [AG_TITAN]   = { &bosses_img,    4, 3, 2 },   /* the blue skull  */
            [AG_MEDUSA]  = { &bosses_img,    8, 1, 2 },   /* green tentacles */
            [AG_REAPER]  = { &bosses_img,   10, 1, 2 },   /* skeleton, sword */
            [AG_PHOENIX] = { &bosses_img,    2, 3, 2 },   /* the gold flame  */
            [AG_GOLEM]   = { &bosses_img,    2, 1, 2 },   /* grey armour     */
            [AG_EYE]     = { &bosses_img,    6, 1, 2 },   /* the great eye   */
            [AG_ANGEL]   = { &bosses_img,    0, 5, 2 },   /* winged, gilded  */
            [AG_MAW]     = { &bosses_img,    0, 1, 2 },   /* the red maw     */
        };
        const AgSpr *a = &AG_SPR[kind >= 0 && kind < AG_N ? kind : AG_TORNADO];
        if (!a->img) continue;
        int px = ax * TILE, py = ay * TILE;
        int wpx = a->w * TILE;
        if (a->w > 1) { px -= TILE / 2; py -= TILE; }
        MoteSprite spr = { a->img, (int16_t)px, (int16_t)(py - TILE),
                           (uint16_t)(a->cx * TILE), (uint16_t)(a->cy * TILE),
                           (uint16_t)wpx, (uint16_t)wpx, 70, 0 };
        add(&spr);
    }
    /* flux, on top of the ground it is consuming */
    /* NO FLUX TILE SPRITES. There used to be one 8x8 sprite per burning, flooded or
     * frozen cell, so a firestorm rendered as a grid of identical explosion icons snapped
     * to the terrain lattice — the effect was as blocky as the ground under it. The flux
     * is drawn as PIXEL PARTICLES now (mb_fx_flux_emit / mb_fx_draw_mortal_px), so a fire
     * front is a cloud of embers that does not know the tile grid exists. Lava keeps a
     * sprite, but as TERRAIN rather than as an effect — it is molten rock you can walk
     * into, not a puff of something. */

    /* WHAT A FIRE LEAVES: lava, scorched earth and ash, as BLOB 47 PATCHES.
     *
     * These three are the band that would not fit — eight layers hold eight bands and
     * there is no ninth — so they are drawn as sprites over whatever band they sit on.
     * They are the right ones to move: measured at 0.0-0.6% of a world, and they always
     * carry flux and FX particles anyway.
     *
     * But one opaque cell stamped per world cell pins the patch boundary to the TILE GRID,
     * so a burn scar came out as a rectilinear blob of one flat dark tone — "it looks like
     * black squares", and the squares were the bigger half of that complaint. So the cell
     * is chosen the way the terrain bands choose theirs: from which of the eight
     * neighbours are the same material, through MB_B47_SAME, with every open side fringed
     * so the ground it burnt shows through at the rim.
     *
     * ASH IS HERE NOW TOO, and it had a worse bug than a square edge: it was a member of
     * the ROCK band, so a burnt forest drew as LIGHT GREY STONE in Mortal View while God's
     * Eye drew it dark. The two views disagreed about what a fire had done. */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t b = mb_w.biome[AT(c, r)];
            int blk;
            if      (b == B_LAVA)     blk = 0;      /* row blocks, in burnt.py's ORDER */
            else if (b == B_SCORCHED) blk = 1;
            else if (b == B_ASH)      blk = 2;
            else continue;

            /* the mask: does this material carry on that way? Off-map counts as yes, so a
             * scar running off the edge of the world is not fringed against nothing. */
            int m = 0;
            static const int8_t NX[8] = {  0,  1, 1, 1,  0, -1, -1, -1 };
            static const int8_t NY[8] = { -1, -1, 0, 1,  1,  1,  0, -1 };
            for (int k = 0; k < 8; k++) {
                int nx = c + NX[k], ny = r + NY[k];
                if (!mb_in(nx, ny) || mb_w.biome[AT(nx, ny)] == b) m |= 1 << k;
            }
            int cell = MB_B47_SAME[m];
            int v = (int)(mb__hash2(c, r) % 3u);        /* three textures, position-picked */
            int row = (blk * 3 + v) * MB_B47_ROWS + cell / MB_B47_COLS;
            MoteSprite spr = { &hot_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               (uint16_t)((cell % MB_B47_COLS) * TILE),
                               (uint16_t)(row * TILE), TILE, TILE, 13, 0 };
            add(&spr);
        }
    }


    /* ROADS, over the terrain and under everything else. The cell is chosen from the
     * FOUR-NEIGHBOUR MASK, because a road is a graph and not an area: what matters is
     * only which cardinals it continues into. Sixteen cells cover every case exactly —
     * a stone, four dead ends, two straights, four corners, four tees and a crossroads
     * — where a 47-cell blob set would be answering the wrong question.
     *
     * AND THE SHEET HAS FIVE ROWS, one per surface: beaten track, cobbles, flagstones,
     * tarmac, and tarmac with a dashed centre line. The row comes from the tech of the
     * kingdom that OWNS the cell, read at draw time — so a realm's entire network
     * modernises the moment its masons or its engineers or its motorists arrive, without a
     * byte of storage per cell and without anyone having to rebuild anything. A road
     * running through nobody's land stays a track, which is also correct.
     */
    for (int r = r0; r <= r0 + MVH; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0; c <= c0 + MVW; c++) {
            if (c < 0 || c >= MW || !mb_w.road[AT(c, r)]) continue;
            int m = 0;
            if (r > 0      && mb_w.road[AT(c, r - 1)]) m |= 1;
            if (c < MW - 1 && mb_w.road[AT(c + 1, r)]) m |= 2;
            if (r < MH - 1 && mb_w.road[AT(c, r + 1)]) m |= 4;
            if (c > 0      && mb_w.road[AT(c - 1, r)]) m |= 8;

            /* the grade: the best surface this cell's owner knows how to lay */
            int grade = 0;
            {
                int ov = mb_w.claim[AT(c, r)];
                int ok = (ov && ov < MAXV && mb_v[ov].alive) ? mb_v[ov].kingdom : 0;
                /* Each surface needs a WINDOW to be seen in. Gating tarmac on steam and the
                 * markings on combustion skipped plain tarmac entirely — measured across five
                 * sample years, grade 3 was never once drawn — because a kingdom with steam
                 * has combustion within a generation. Spaced out across the tree, every one
                 * of the five gets its own era. */
                if (mb_tech_known(ok, TECH_FLIGHT))         grade = 4;   /* lines painted on */
                else if (mb_tech_known(ok, TECH_COMBUSTION))grade = 3;   /* asphalt          */
                else if (mb_tech_known(ok, TECH_ENGINEER))  grade = 2;   /* dressed flags    */
                else if (mb_tech_known(ok, TECH_MASONRY))   grade = 1;   /* set cobbles      */
            }
            MoteSprite spr = { &road_img, (int16_t)(c * TILE), (int16_t)(r * TILE),
                               (uint16_t)(m * TILE), (uint16_t)(grade * TILE),
                               TILE, TILE, 12, 0 };
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
