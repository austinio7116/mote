/*
 * Motebox — world generation.
 *
 * Integer-only, and deliberately so: the whole sim is reproducible from
 * (seed, tick), which is what makes the headless audit meaningful, replays
 * possible and the two-god link mode in DESIGN.md 19 buildable later. Floats
 * are allowed in FX and rendering, nowhere here.
 *
 * Elevation and moisture come from the same value-noise generator at different
 * octave sets; temperature is latitude plus a lapse rate off elevation. Biome
 * is then a lookup on (elevation, temperature, moisture) — a Whittaker diagram
 * with the height bands laid over it.
 */
#include "mb.h"
#include <string.h>
#if MOTE_HOST
#include <stdio.h>
#endif

World mb_w;
const MoteApi *g_api;

/* --- world shape and climate -------------------------------------------
 * Latitude alone made every seed the same picture: white cap, green belt, white
 * cap, one fat continent. Two rolls off the seed fix that, and they are what
 * makes re-rolling a world worth doing — a shape (how much of the map is sea and
 * how big the landmasses are) and a climate (where the temperature band sits).
 * A cold archipelago and a hot pangaea are different games. */
enum { SH_ARCHIPELAGO, SH_ISLES, SH_CONTINENT, SH_PANGAEA, SH_N };
enum { CL_ICE, CL_TEMPERATE, CL_HOT, CL_N };

static const char *const SH_NAME[SH_N] = { "ARCHIPELAGO", "ISLES", "CONTINENT", "PANGAEA" };
static const char *const CL_NAME[CL_N] = { "FROZEN", "TEMPERATE", "SCORCHING" };

/* sea level, noise period, coastline sharpening (shift), edge falloff strength */
static const uint8_t SH_SEA[SH_N]    = { 142, 132, 121, 110 };
static const uint8_t SH_PERIOD[SH_N] = {  26,  38,  52,  72 };
static const uint8_t SH_SHARP[SH_N]  = {   3,   2,   2,   2 };  /* >> shift */
static const uint8_t SH_EDGE[SH_N]   = {  90,  80,  70,  55 };

/* equator temperature, and how much it falls to the pole */
static const uint8_t CL_BASE[CL_N] = { 200, 238, 255 };
static const uint8_t CL_SPAN[CL_N] = { 205, 185, 165 };

const char *mb_world_shape_name(void)   { return SH_NAME[mb_w.shape]; }
const char *mb_world_climate_name(void) { return CL_NAME[mb_w.climate]; }

int mb_sea_level(void) { return mb_w.sea; }

/* --- integer value noise ------------------------------------------------ */

static uint32_t h32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

/* lattice value at (gx,gy) for one octave, 0..255 */
static int lat(int gx, int gy, uint32_t seed) {
    return (int)(h32((uint32_t)gx * 374761393u + (uint32_t)gy * 668265263u + seed) >> 24);
}

/* Smoothstep in 8.8 fixed point: 3t^2 - 2t^3 with t in 0..256. */
static int smooth(int t) {
    int t2 = (t * t) >> 8;
    int t3 = (t2 * t) >> 8;
    return (3 * t2 - 2 * t3);
}

/* One octave of bilinear value noise with period p, sampled at (x,y). 0..255 */
static int oct(int x, int y, int p, uint32_t seed) {
    int gx = x / p, gy = y / p;
    int fx = ((x % p) << 8) / p, fy = ((y % p) << 8) / p;
    int sx = smooth(fx), sy = smooth(fy);
    int a = lat(gx,     gy,     seed), b = lat(gx + 1, gy,     seed);
    int c = lat(gx,     gy + 1, seed), d = lat(gx + 1, gy + 1, seed);
    int top = a + (((b - a) * sx) >> 8);
    int bot = c + (((d - c) * sx) >> 8);
    return top + (((bot - top) * sy) >> 8);
}

/* Fractal sum, normalised back to 0..255. */
static int fbm(int x, int y, uint32_t seed, int p0, int octaves) {
    int v = 0, amp = 128, norm = 0, p = p0;
    for (int i = 0; i < octaves; i++) {
        v += oct(x, y, p < 2 ? 2 : p, seed + (uint32_t)i * 7919u) * amp;
        norm += amp; amp >>= 1; p >>= 1;
    }
    return v / (norm ? norm : 1);
}

/* --- shaping ------------------------------------------------------------ */

/* Pull the map edges under water so the world always reads as islands framed by
 * sea, which is what makes the God's Eye view a picture rather than a crop. */
static int edge_falloff(int x, int y) {
    int dx = x < MW - 1 - x ? x : MW - 1 - x;
    int dy = y < MH - 1 - y ? y : MH - 1 - y;
    int d  = dx < dy ? dx : dy;
    const int margin = 16;
    if (d >= margin) return 0;
    int t = margin - d;                   /* 1..margin */
    return (t * t * SH_EDGE[mb_w.shape]) / (margin * margin);
}

/* Latitude temperature: hot at the equator row, cold at the poles, minus a
 * lapse rate with height so mountains are cold wherever they are. */
static int temp_at(int y, int elev) {
    int mid = MH / 2;
    int d = y > mid ? y - mid : mid - y;                     /* 0 .. MH/2 */
    int t = CL_BASE[mb_w.climate] - (d * CL_SPAN[mb_w.climate]) / (MH / 2);
    int above = elev > mb_w.sea ? elev - mb_w.sea : 0;
    t -= above * 5 / 4;                                      /* lapse rate */
    return t < 0 ? 0 : t > 255 ? 255 : t;
}

/* --- rivers ------------------------------------------------------------- */

/* Walk downhill from a high cell, carving shallow water, until we hit the sea
 * or stop descending. Rivers are what make a continent legible at 1 px/tile:
 * they draw the drainage the elevation already implies. */
static void carve_river(int x, int y)
{
    uint8_t *e = mb_w.elev, *b = mb_w.biome;
    for (int step = 0; step < 400; step++) {
        if (!mb_in(x, y)) return;
        if (mb_water(b[AT(x, y)])) return;                 /* reached water */
        b[AT(x, y)] = B_SHALLOW;
        int bx = -1, by = -1, best = e[AT(x, y)];
        static const int8_t DX[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
        static const int8_t DY[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
        for (int k = 0; k < 8; k++) {
            int nx = x + DX[k], ny = y + DY[k];
            if (!mb_in(nx, ny)) continue;
            if (e[AT(nx, ny)] < best) { best = e[AT(nx, ny)]; bx = nx; by = ny; }
        }
        if (bx < 0) return;                                /* a basin: stop */
        x = bx; y = by;
    }
}

/* The opening cursor cell: the centroid of the biggest landmass, nudged to the
 * nearest land tile. Opening on open ocean would be the first thing a player
 * saw, and it says nothing about the world. */
void mb_world_start(int *ox, int *oy)
{
    /* No flood fill needed for a centroid of mass — the biggest landmass
     * dominates the sum, and a nudge fixes the rest. */
    long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++)
            if (mb_land(mb_w.biome[AT(x, y)])) { sx += x; sy += y; n++; }
    int cx = n ? (int)(sx / n) : MW / 2, cy = n ? (int)(sy / n) : MH / 2;
    if (n && mb_land(mb_w.biome[AT(cx, cy)])) { *ox = cx; *oy = cy; return; }
    for (int r = 1; r < MW; r++) {                 /* spiral out to real land */
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy < (r - 1) * (r - 1)) continue;
                int x = cx + dx, y = cy + dy;
                if (mb_in(x, y) && mb_land(mb_w.biome[AT(x, y)])) { *ox = x; *oy = y; return; }
            }
    }
    *ox = cx; *oy = cy;
}

/* --- generation --------------------------------------------------------- */

void mb_world_alloc(void)
{
    mb_w.biome = (uint8_t *)g_api->alloc(NC);
    mb_w.elev  = (uint8_t *)g_api->alloc(NC);
    mb_w.obj   = (uint8_t *)g_api->alloc(NC);
    mb_w.flux  = (uint8_t *)g_api->alloc(NC);
    mb_w.claim = (uint8_t *)g_api->alloc(NC);
}

void mb_world_gen(uint32_t seed)
{
    uint8_t *bio = mb_w.biome, *elv = mb_w.elev, *ob = mb_w.obj;
    mb_w.seed = seed ? seed : 1u;
    mb_w.tick = 0;

    /* the two rolls that make a seed a world rather than a variation */
    uint32_t r = h32(mb_w.seed ^ 0x2f6a88b3u);
    mb_w.shape   = (uint8_t)(r % SH_N);
    mb_w.climate = (uint8_t)((r >> 8) % CL_N);
    mb_w.sea     = SH_SEA[mb_w.shape];
    memset(mb_w.flux,  0, NC);
    memset(mb_w.claim, 0, NC);
    memset(ob,         0, NC);

    /* 1. elevation */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int h = fbm(x, y, mb_w.seed, SH_PERIOD[mb_w.shape], 5);
            /* Sharpen around sea level so the coastline is decisive instead of a
             * fringe of one-tile islets; an archipelago wants it gentler, so the
             * shift comes from the shape. */
            h = h + ((h - mb_w.sea) >> SH_SHARP[mb_w.shape]);
            h -= edge_falloff(x, y);
            elv[AT(x, y)] = (uint8_t)(h < 0 ? 0 : h > 255 ? 255 : h);
        }
    }

    /* 2. moisture + biome */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int e = elv[AT(x, y)];
            int m = fbm(x, y, mb_w.seed ^ 0x9e3779b9u, 32, 4);
            int t = temp_at(y, e);
            uint8_t b;
            int sea = mb_w.sea;
            if      (e < sea - 26) b = B_OCEAN;
            else if (e < sea -  9) b = B_SEA;
            else if (e < sea)      b = (t < 45) ? B_ICE : B_SHALLOW;
            else if (e < sea +  4) b = (t < 45) ? B_SNOW : B_BEACH;
            else if (e > 214)      b = B_PEAK;
            else if (e > 196)      b = B_MOUNTAIN;
            else if (e > 172)      b = (t < 70) ? B_TUNDRA : B_HILL;
            else if (t <  45)      b = B_SNOW;
            else if (t <  85)      b = (m > 150) ? B_FOREST : B_TUNDRA;
            else if (t < 175) {                              /* temperate */
                if      (m > 205) b = B_SWAMP;
                else if (m > 160) b = B_FOREST;
                else if (m > 110) b = B_MEADOW;
                else              b = B_GRASS;
            } else {                                         /* hot */
                if      (m > 200) b = B_SWAMP;
                else if (m > 150) b = B_MEADOW;
                else if (m >  95) b = B_SAVANNA;
                else              b = B_DESERT;
            }
            bio[AT(x, y)] = b;
        }
    }

    /* 3. rivers from the highest cells */
    mote_rand_seed(mb_w.seed ^ 0x5bf03635u);
    for (int tries = 0, made = 0; tries < 900 && made < 14; tries++) {
        int x = (int)(mote_rand() % MW), y = (int)(mote_rand() % MH);
        if (elv[AT(x, y)] < 180) continue;
        carve_river(x, y); made++;
    }

    /* 4. scatter: vegetation by biome, ore in the rock, so the world has
     * something to gather before anyone is there to gather it. */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            uint8_t b = bio[AT(x, y)];
            uint32_t r = h32((uint32_t)AT(x, y) * 2654435761u + mb_w.seed);
            int roll = (int)(r & 255), pick = (int)((r >> 8) & 255);
            switch (b) {
            case B_FOREST:  if (roll < 150) ob[AT(x,y)] = pick & 1 ? O_TREE : O_TREE2; break;
            case B_MEADOW:  if (roll <  46) ob[AT(x,y)] = pick & 1 ? O_TREE : O_BUSH;
                            else if (roll < 92) ob[AT(x,y)] = O_TUFT; break;
            case B_GRASS:   if (roll <  22) ob[AT(x,y)] = O_TUFT;
                            else if (roll < 30) ob[AT(x,y)] = O_FLOWER;
                            else if (roll < 34) ob[AT(x,y)] = O_TREE; break;
            case B_SWAMP:   if (roll <  60) ob[AT(x,y)] = pick & 1 ? O_DEAD : O_REEDS; break;
            case B_SAVANNA: if (roll <  18) ob[AT(x,y)] = O_TUFT;
                            else if (roll < 26) ob[AT(x,y)] = O_DEAD; break;
            case B_DESERT:  if (roll <  20) ob[AT(x,y)] = O_ROCK; break;
            case B_HILL:    if (roll <  30) ob[AT(x,y)] = O_ROCK;
                            else if (roll < 40) ob[AT(x,y)] = O_ORE;
                            else if (roll < 44) ob[AT(x,y)] = O_SILVER; break;
            case B_MOUNTAIN:if (roll <  60) ob[AT(x,y)] = O_BOULDER;
                            else if (roll < 72) ob[AT(x,y)] = O_ORE;
                            else if (roll < 78) ob[AT(x,y)] = O_GOLD;
                            else if (roll < 81) ob[AT(x,y)] = O_GEM; break;
            case B_PEAK:    if (roll <  90) ob[AT(x,y)] = O_PEAKROCK; break;
            case B_TUNDRA:  if (roll <  14) ob[AT(x,y)] = O_ROCK; break;
            case B_SNOW:    if (roll <  10) ob[AT(x,y)] = O_DEAD; break;
            default: break;
            }
        }
    }
}

/* --- worldgen stats -----------------------------------------------------
 * MOTEBOX_STAT=1 prints the shape of a generated world, because "does it look
 * right" is not a check. Land fraction and the biome histogram catch the two
 * failures that eyeballing a 128x112 thumbnail misses: a world that is 70% land
 * (so the sea powers and docks have nothing to do) and a climate band so wide
 * that a third of the map is polar. */
void mb_world_stats(void)
{
#if MOTE_HOST
    int hist[B_N] = { 0 }, land = 0, obj = 0;
    for (int i = 0; i < NC; i++) {
        uint8_t b = mb_w.biome[i];
        if (b < B_N) hist[b]++;
        if (mb_land(b)) land++;
        if (mb_w.obj[i]) obj++;
    }
    fprintf(stderr, "world seed=0x%08x %s/%s sea=%d land=%d%% objects=%d%%\n",
            mb_w.seed, mb_world_shape_name(), mb_world_climate_name(),
            mb_w.sea, land * 100 / NC, obj * 100 / NC);
    /* the five biggest biomes, which is what the map actually reads as */
    for (int rank = 0; rank < 5; rank++) {
        int best = 0;
        for (int b = 1; b < B_N; b++) if (hist[b] > hist[best]) best = b;
        if (!hist[best]) break;
        fprintf(stderr, "   %2d: %3d%%\n", best, hist[best] * 100 / NC);
        hist[best] = 0;
    }
#endif
}

/* --- regrowth ------------------------------------------------------------
 * Without this the world is one-way: every fire is permanent, every acid bay
 * final, and after twenty minutes any world is grey. WorldBox regrows, and it is
 * what makes a disaster something you can do TWICE.
 *
 * A sample of cells per tick rather than a sweep: 48 of 14336 is a full pass
 * every 300 ticks (about six years), which is the right pace for a forest — and
 * it costs the same whether the world is pristine or ruined.
 */
/* Graves weather on their own sweep, not on the random sample.
 *
 * WHY SEPARATELY. The regrowth sample visits 48 of 14336 cells a tick, so a cell
 * waits ~300 ticks to be looked at; at any per-visit chance that reads as
 * plausible, a headstone stood for a century of game time and every settled
 * village silted up with them. A rotating cursor covers the whole map every 64
 * ticks instead, so removal scales with how many graves there ARE, and a grave
 * lasts about a decade — long enough that a battlefield is still readable the
 * next time you look, short enough that peace clears it. */
void mb_grave_step(void)
{
    static uint32_t cur;
    for (int n = 0; n < NC / 64; n++) {
        uint32_t i = cur++ % NC;
        if (mb_w.obj[i] != O_GRAVE) continue;
        if ((mb_rand(i * 2246822519u + 0x51edu) & 255) < 26) mb_w.obj[i] = O_NONE;
    }
}

void mb_grow_step(void)
{
    for (int t = 0; t < 48; t++) {
        uint32_t r = mb_rand((uint32_t)t * 2654435761u + 0x9e37u);
        int x = (int)(r % MW), y = (int)((r >> 11) % MH);
        int i = AT(x, y);
        uint8_t b = mb_w.biome[i], o = mb_w.obj[i];
        int roll = (int)((r >> 22) & 255);

        /* 1. scarred ground heals up the ladder it was pushed down */
        switch (b) {
        case B_SCORCHED: if (roll < 40) mb_w.biome[i] = B_ASH;    continue;
        /* ASH HEALS FASTER THAN IT USED TO. At 30/256 on a sample that revisits a
         * cell every ~300 ticks, a burnt tile lasted about fifty years, so a town
         * two centuries after its last fire was still speckled with grey. */
        case B_ASH:      if (roll < 90) mb_w.biome[i] = B_GRASS;  continue;
        case B_RUBBLE:   if (roll < 25) mb_w.biome[i] = B_HILL;   continue;
        case B_LAVA:     continue;                      /* only the CA cools lava */
        default: break;
        }

        /* 2. graves are swept separately (see mb_grave_step) — the 48-cell sample
         * revisits a given cell about every 300 ticks, and at any believable
         * per-visit chance that left headstones standing for over a century. */
        if (o == O_GRAVE) continue;

        /* 3. vegetation returns, but only next to vegetation — so a burnt
         * continent regrows inward from its surviving edges instead of sprouting
         * evenly, which is both true and much better to watch */
        if (o != O_NONE) continue;
        int neigh = 0;
        for (int k = 0; k < 4; k++) {
            static const int8_t DX[4] = { 1, -1, 0, 0 };
            static const int8_t DY[4] = { 0, 0, 1, -1 };
            int nx = x + DX[k], ny = y + DY[k];
            if (!mb_in(nx, ny)) continue;
            uint8_t no = mb_w.obj[AT(nx, ny)];
            if (no == O_TREE || no == O_TREE2 || no == O_BUSH) neigh++;
        }
        int fert = mb_age_fertility();
        switch (b) {
        case B_FOREST:
            if (roll * 100 < 70 * fert && neigh) mb_w.obj[i] = (r & 1) ? O_TREE : O_TREE2;
            break;
        case B_MEADOW:
            if (roll * 100 < 40 * fert) mb_w.obj[i] = neigh ? O_BUSH : O_TUFT;
            break;
        case B_GRASS:
            if (roll * 100 < 25 * fert) mb_w.obj[i] = (roll & 1) ? O_TUFT : O_BUSH;
            /* grass next to forest becomes forest: woodland advances */
            if (neigh >= 2 && roll < 6) mb_w.biome[i] = B_FOREST;
            break;
        case B_SAVANNA:
            if (roll * 100 < 18 * fert) mb_w.obj[i] = O_TUFT;
            break;
        case B_DESERT:
            if (roll < 4) mb_w.obj[i] = O_ROCK;
            break;
        case B_SWAMP:
            if (roll * 100 < 30 * fert) mb_w.obj[i] = (roll & 1) ? O_DEAD : O_REEDS;
            break;
        default: break;
        }
    }
}
