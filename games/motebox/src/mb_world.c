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
#include <math.h>
#if MOTE_HOST
#include <stdlib.h>
#include <stdio.h>
#endif
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

/* --- RIVERS, BY FLOW ACCUMULATION ---------------------------------------
 *
 * The old generator walked steepest-descent from fourteen random cells above elevation 180,
 * painting one cell of shallow water per step. It produced scratches, and every reason is
 * structural rather than a matter of tuning:
 *
 *   ONE CELL WIDE, EVERYWHERE. A stream and the river it feeds looked identical, so nothing
 *     on the map said which way the water went or how much of it there was.
 *   NO TRIBUTARIES AND NO CONFLUENCES. Fourteen independent walks that happen to cross just
 *     overwrite each other, so a continent had fourteen unrelated lines on it instead of a
 *     drainage network — and a network is the thing that makes a landscape look explained.
 *   THEY STOPPED IN BASINS. "No lower neighbour" ended the walk, so rivers petered out in
 *     the middle of the continent, going nowhere.
 *   THEY DID NOT CUT. The channel was painted over the elevation field without lowering it,
 *     so with the hillshade in place a river could be seen running along the top of a ridge.
 *
 * This is the standard hydrology instead, and it is cheap. Every land cell starts with one
 * unit of rain; sweeping elevation levels from 255 down, each cell pushes its accumulated
 * flow into its steepest downhill neighbour. Because a cell is only ever visited after every
 * cell that could drain into it, one pass is exact — no iteration, no relaxation.
 *
 * What falls out for free is everything the old one lacked: tributaries join and their flows
 * ADD, so width grows downstream; the network is a tree rooted at the sea; a basin becomes a
 * LAKE rather than a dead end; and rivers sit in valleys because they are derived from the
 * valleys. Then the channel is cut a few units deep so the relief agrees with the water.
 *
 * The 256 sweeps ARE the sort. Bucketing by elevation costs no memory and no index array,
 * which matters because there is none to spare — the accumulator borrows mb_w.claim, which
 * is zeroed at the start of generation and owned by nobody until villages exist. It is put
 * back as it was found.
 */
#define RIV_STREAM 20        /* a stream: one cell wide */
#define RIV_RIVER  70        /* a river: three cells */
#define RIV_GREAT 170        /* a great river: five, and a delta at the mouth */

static const int8_t RDX[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
static const int8_t RDY[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };

static void river_cell(int x, int y, int cut)
{
    if (!mb_in(x, y)) return;
    int i = AT(x, y);
    if (!mb_land(mb_w.biome[i])) return;
    mb_w.biome[i] = B_SHALLOW;
    mb_w.obj[i]   = O_NONE;
    /* CUT THE CHANNEL. Without this the hillshade lights the ground a river runs over as
     * though it were dry, and the water reads as paint rather than as a valley floor. */
    if (mb_w.elev[i] > cut) mb_w.elev[i] = (uint8_t)(mb_w.elev[i] - cut);
}

/* --- PRIORITY FLOOD: the fill, done properly ----------------------------
 *
 * Two cheap fills were tried and both are documented failures worth remembering, because
 * each produced a distinctive and misleading artefact on screen:
 *
 *   RAISE TO THE RIM flattens the landscape. A cell raised to its lowest neighbour has no
 *     strictly-lower neighbour, and the flatness spreads: 8474 of 12,743 traces then had
 *     nowhere to go at all.
 *   RAISE TO THE RIM PLUS ONE oscillates. A rises above B, B rises above A, and the stable
 *     state is a SAWTOOTH. Printing one row of elevations showed it at once — 135 140 143
 *     146 148 140 149 144 147 139 — and the rivers were faithfully following a checkerboard
 *     of artificial local lows, hatching every plain with alternate-cell diagonals. Two
 *     rounds of screenshots could not explain that; twenty numbers did.
 *
 * The real algorithm is a priority flood, and it is O(n) here because elevations are BYTES:
 * a 256-bucket queue is an exact priority queue. Start from the sea, always expand the
 * lowest frontier cell, and give each cell it discovers max(its own height, the frontier
 * cell's filled height). Pits vanish, flats come out as genuine plateaus at their true spill
 * level rather than as sawteeth, and — the part that matters most — THE DISCOVERY ORDER IS A
 * DRAINAGE ORDER. Recording which neighbour discovered each cell gives a spanning tree
 * rooted in the ocean, so every cell has one unambiguous downstream neighbour, no cycles are
 * possible and flats need no special handling at all.
 *
 * It needs four byte maps and one 16-bit map of scratch, and it gets them for free: at this
 * point in generation biome and elevation are the only maps with anything in them. obj, flux,
 * claim, road and layer are all still zero and are handed back below.
 */
uint8_t *mb_river_fill;      /* the filled surface, for flow_down() */

#define PF_NONE 255

static void priority_flood(uint8_t *fill, uint8_t *parent, uint8_t *qlo, uint8_t *qhi)
{
    const uint8_t *e = mb_w.elev, *b = mb_w.biome;
    /* 256 singly-linked bucket lists over cell indices; 0xFFFF terminates. */
    uint16_t head[256];
    for (int k = 0; k < 256; k++) head[k] = 0xFFFF;
    #define QPUT(idx, lev) do {                                            \
        qlo[idx] = (uint8_t)(head[lev] & 0xFF);                            \
        qhi[idx] = (uint8_t)(head[lev] >> 8);                              \
        head[lev] = (uint16_t)(idx);                                       \
    } while (0)
    #define QNEXT(idx) ((uint16_t)(qlo[idx] | (qhi[idx] << 8)))

    for (int i = 0; i < NC; i++) { parent[i] = PF_NONE; fill[i] = 0; }

    /* every water cell is an outlet, and so is the map edge */
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            int i = AT(x, y);
            int edge = (x == 0 || y == 0 || x == MW - 1 || y == MH - 1);
            if (!mb_land(b[i]) || edge) {
                fill[i] = e[i]; parent[i] = 8;          /* 8 = the sea: a root */
                QPUT(i, e[i]);
            }
        }

    for (int lev = 0; lev < 256; lev++) {
        /* A cell can only ever be pushed into a bucket at or above the level being drained,
         * so one forward pass over the buckets is a complete Dijkstra. */
        while (head[lev] != 0xFFFF) {
            int c = head[lev];
            head[lev] = QNEXT(c);
            int cx = c % MW, cy = c / MW;
            for (int k = 0; k < 8; k++) {
                int nx = cx + RDX[k], ny = cy + RDY[k];
                if (!mb_in(nx, ny)) continue;
                int ni = AT(nx, ny);
                if (parent[ni] != PF_NONE) continue;    /* already drained */
                int h = e[ni] > fill[c] ? e[ni] : fill[c];
                fill[ni] = (uint8_t)h;
                /* THE OPPOSITE DIRECTION. k points from c to the neighbour, but what the
                 * neighbour needs to record is the way to ITS outlet, which is back toward
                 * c. Storing k walked the tree upstream: every trace cycled and 12,499 of
                 * 12,743 hit the step cap. */
                static const uint8_t OPP[8] = { 1, 0, 3, 2, 7, 6, 5, 4 };
                parent[ni] = OPP[k];
                QPUT(ni, h < lev ? lev : h);            /* never behind the sweep */
            }
        }
    }
    #undef QPUT
    #undef QNEXT
}

/* The downstream neighbour: STEEPEST DESCENT ON THE FILLED SURFACE, with the flood's
 * spanning tree only as the fallback.
 *
 * Using the tree alone gave DEAD STRAIGHT rivers running east-west across the whole map. The
 * tree is valid — every cell has a parent and every path reaches the sea — but its shape comes
 * from the order the flood discovered cells, and the bucket queue is a stack scanned row-major,
 * so on any ground that is not steeply sloped the parents all point along a row. Thresholding
 * by flow hid that, because flow accumulates wherever the terrain does happen to lead; tracing
 * the tree exposed it immediately.
 *
 * So the terrain leads. The filled surface has no pits, so almost everywhere there is a
 * strictly-lower neighbour and following the steepest one draws the valley. The only cells
 * without one are on the flats the fill created — the surfaces of filled lakes — and there the
 * tree takes over, which is both a guarantee of progress and the right answer: a lake's outflow
 * really does run more or less straight across it.
 */
static int flow_down(int x, int y, const uint8_t *parent, int *ox, int *oy)
{
    extern uint8_t *mb_river_fill;
    const uint8_t *fl = mb_river_fill, *e = mb_w.elev;
    int here = fl[AT(x, y)];
    int best = -1, bestlev = here, bestraw = 256;

    for (int k = 0; k < 8; k++) {
        int nx = x + RDX[k], ny = y + RDY[k];
        if (!mb_in(nx, ny)) continue;
        int ni = AT(nx, ny);
        if (fl[ni] > bestlev) continue;
        /* strictly lower wins; on a tie the lower ORIGINAL ground wins, which is what keeps a
         * channel in the bottom of its valley instead of wandering across the floor */
        if (fl[ni] < bestlev || (fl[ni] == bestlev && best >= 0 && e[ni] < bestraw)) {
            if (fl[ni] == bestlev && best < 0) continue;      /* level with us is not downhill */
            bestlev = fl[ni]; bestraw = e[ni]; best = ni; *ox = nx; *oy = ny;
        }
    }
    if (best >= 0) return best;

    int p = parent[AT(x, y)];                     /* across a filled lake, take the tree */
    if (p >= 8) return -1;
    int nx = x + RDX[p], ny = y + RDY[p];
    if (!mb_in(nx, ny)) return -1;
    *ox = nx; *oy = ny;
    return AT(nx, ny);
}

/* THE THRESHOLD IS MEASURED PER WORLD, not fixed. A pangaea has 12,743 land cells and an
 * archipelago 3,358 — measured — so any absolute flow cut that gives a continent a sensible
 * river network drowns one world and leaves the other dry. This takes the flow histogram and
 * returns the value at which the requested fraction of the land is wet, so "rivers are about
 * one and a half per cent of the land" holds whatever shape the world came out. */
static int flow_cut(const uint8_t *lo, const uint8_t *hi, int land, int permille)
{
    int hist[256];
    for (int k = 0; k < 256; k++) hist[k] = 0;
    for (int i = 0; i < NC; i++) {
        if (!mb_land(mb_w.biome[i])) continue;
        int f = (hi[i] << 8) | lo[i];
        int bin = f >> 4;                          /* sixteen units a bin is plenty for a cut */
        hist[bin > 255 ? 255 : bin]++;
    }
    int want = land * permille / 1000, acc = 0;
    if (want < 1) want = 1;
    for (int bin = 255; bin >= 1; bin--) {
        acc += hist[bin];
        if (acc >= want) return bin << 4;
    }
    return 16;
}

static void rivers_build(void)
{
    /* Five byte maps borrowed; every one is zero at this point in generation and every one is
     * handed back at the end, so the whole drainage network costs no permanent memory. */
    uint8_t *fill   = mb_w.layer;
    mb_river_fill = fill;
    uint8_t *parent = mb_w.obj;
    uint8_t *lo     = mb_w.claim;     /* the bucket queue's links, then the flow's low byte */
    uint8_t *hi     = mb_w.road;      /* ditto, high byte — 255 was not enough: 2.8% of a  */
    uint8_t *b      = mb_w.biome;     /* pangaea pinned at the cap, so a main stem and a   */
                                      /* middling tributary were indistinguishable.        */
    priority_flood(fill, parent, lo, hi);

    for (int i = 0; i < NC; i++) { lo[i] = 0; hi[i] = 0; }

    int land = 0;
#if MOTE_HOST
    int reached = 0, stalled = 0, capped = 0;
#endif
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            if (!mb_land(b[AT(x, y)])) continue;
            land++;
            int cx = x, cy = y, step = 0;
            for (; step < 400; step++) {
                int i = AT(cx, cy);
                if (lo[i] == 255) { if (hi[i] < 255) { hi[i]++; lo[i] = 0; } }
                else lo[i]++;
                int nx = cx, ny = cy;
                if (flow_down(cx, cy, parent, &nx, &ny) < 0) break;
                cx = nx; cy = ny;
                if (!mb_land(b[AT(cx, cy)])) break;      /* the coast: done */
            }
#if MOTE_HOST
            if (step >= 400) capped++;
            else if (mb_in(cx, cy) && !mb_land(b[AT(cx, cy)])) reached++;
            else stalled++;
#endif
        }
#if MOTE_HOST
    if (getenv("MOTEBOX_STAT"))
        fprintf(stderr, "rivers: traces %d -> sea %d, stalled %d, capped %d\n",
                land, reached, stalled, capped);
#endif

    /* One and a half per cent of the land carries a stream, a third of that is a river and a
     * fifth of that a great river. The cut is measured per world because a pangaea has 12,743
     * land cells and an archipelago 3,358 — any fixed flow threshold drowns one and leaves
     * the other dry. */
    const int cut_s = flow_cut(lo, hi, land, 15);
    const int cut_r = flow_cut(lo, hi, land, 5);
    const int cut_g = flow_cut(lo, hi, land, 1);
#if MOTE_HOST
    if (getenv("MOTEBOX_STAT"))
        fprintf(stderr, "rivers: %d land cells; cuts stream=%d river=%d great=%d\n",
                land, cut_s, cut_r, cut_g);
#endif

    /* --- CARVE BY TRACING, NOT BY THRESHOLDING --------------------------
     *
     * Deciding cell by cell whether the flow clears a threshold is what produced holes. It
     * should not have: flow only ever increases downstream, so the test is monotone along a
     * channel. But any attempt to break up the braiding on flat ground has to REMOVE cells,
     * and removing cells from a line leaves gaps — the thinning rule dropped every cell whose
     * orthogonal neighbour happened to be the same channel two steps ahead, which on a
     * staircase diagonal is most of them. Spotty rivers, spotty ground.
     *
     * So the channel is DRAWN, the way grandthumbauto's citygen draws its rivers: find the
     * heads, walk each one down the drainage tree to the sea, and stamp an overlapping DISC at
     * every step. Overlapping discs cannot leave a hole, whatever the path does, and the
     * radius gives a river body instead of a one-pixel scratch. Braids simply merge into each
     * other, which is what braids look like.
     *
     * The hydrology is still ours and is better than an edge-to-edge sweep for this game: a
     * source sits in high ground, the path follows the valleys the priority flood found, and
     * every river reaches the coast. What is borrowed is only how it is painted — including the
     * slow sinusoidal swell in width, which is the detail that stops a channel reading as a
     * pipe of constant bore.
     */
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            int i = AT(x, y);
            int f = (hi[i] << 8) | lo[i];
            if (!f || f < cut_s) continue;

            /* TRACE FROM EVERY CHANNEL CELL, not just from the heads.
             *
             * Detecting heads — a channel cell that nothing upstream feeds — halves the work
             * and cost most of the network: the head test asked whether a neighbour's
             * downstream cell was this one, and after routing changed to follow the terrain
             * rather than the flood's tree the two no longer agreed, so channels whose head
             * went unrecognised were never drawn at all. Rivers came out as disconnected
             * fragments near the coast.
             *
             * Stamping is idempotent, so tracing every channel cell simply redraws the lower
             * reaches many times over and cannot miss anything. About two hundred sources at
             * a few dozen steps each, once, at world generation.
             */
            int cx = x, cy = y;
            for (int step = 0; step < 400; step++) {
                int ci = AT(cx, cy);
                int cf = (hi[ci] << 8) | lo[ci];

                /* WIDTH. Half a cell for a stream, one and a half for a river, two and a half
                 * for a great one, with a slow swell along the length so no reach is uniform. */
                float base = (cf >= cut_g) ? 2.4f : (cf >= cut_r) ? 1.5f : 0.6f;
                float swell = 0.74f + 0.42f * fabsf(sinf((float)step * 0.21f
                                                         + (float)(x * 7 + y * 3) * 0.11f));
                float w = base * swell;
                int cut = (cf >= cut_g) ? 8 : (cf >= cut_r) ? 6 : 4;
                int ir = (int)(w + 0.5f);
                for (int oy = -ir; oy <= ir; oy++)
                    for (int ox = -ir; ox <= ir; ox++)
                        if ((float)(ox * ox + oy * oy) <= w * w + 0.25f)
                            river_cell(cx + ox, cy + oy, cut);

                int nx = cx, ny = cy;
                int ni = flow_down(cx, cy, parent, &nx, &ny);
                if (ni < 0) {
                    /* A LAKE where the tree has no parent: the water pools rather than
                     * stopping mid-valley, which reads as a mistake. */
                    if (cf >= cut_r) {
                        int lr = (cf >= cut_g) ? 3 : 2;
                        for (int oy = -lr; oy <= lr; oy++)
                            for (int ox = -lr; ox <= lr; ox++)
                                if (ox * ox + oy * oy <= lr * lr) river_cell(cx + ox, cy + oy, cut);
                    }
                    break;
                }
                if (!mb_land(b[ni])) {
                    /* A DELTA at the mouth: a great river should not meet the sea as a slot. */
                    if (cf >= cut_r) {
                        int dr = (cf >= cut_g) ? 3 : 2;
                        for (int oy = -dr; oy <= dr; oy++)
                            for (int ox = -dr; ox <= dr; ox++)
                                if (ox * ox + oy * oy <= dr * dr) river_cell(cx + ox, cy + oy, cut);
                    }
                    break;
                }
                cx = nx; cy = ny;
            }
        }

    for (int i = 0; i < NC; i++) { lo[i] = 0; hi[i] = 0; fill[i] = 0; parent[i] = 0; }
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
    mb_w.road  = (uint8_t *)g_api->alloc(NC);
    mb_w.layer = (uint8_t *)g_api->alloc(NC);
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
    memset(mb_w.road,  0, NC);
    memset(mb_w.layer, 0, NC);
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

    /* 3. rivers: the whole drainage network at once, from the elevation field */
    rivers_build();

    /* 4. scatter: vegetation by biome, ore in the rock, so the world has
     * something to gather before anyone is there to gather it. */
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            uint8_t b = bio[AT(x, y)];
            uint32_t r = h32((uint32_t)AT(x, y) * 2654435761u + mb_w.seed);
            int roll = (int)(r & 255), pick = (int)((r >> 8) & 255);
            int steep = 0;
            if (x > 0 && x < MW - 1 && y > 0 && y < MH - 1) {
                int dx = (int)elv[AT(x + 1, y)] - (int)elv[AT(x - 1, y)];
                int dy = (int)elv[AT(x, y + 1)] - (int)elv[AT(x, y - 1)];
                steep = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            }
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
            case B_HILL:    if (roll <  14) ob[AT(x,y)] = O_ROCK;
                            else if (roll < 20) ob[AT(x,y)] = O_ORE;
                            else if (roll < 22) ob[AT(x,y)] = O_SILVER; break;
            /* ROCK IS SPARSE NOW, and it is placed by SLOPE.
             *
             * Peaks used to scatter a crag on 35% of their cells — 59 of them in one
             * 16x14 view, measured — and a mountain range read as a field of identical
             * pale triangles. The landform is drawn by the hillshade in mb_draw_relief();
             * these are outcrops ON it, so they want to be rare and they want to be
             * where rock would actually break the surface: the steep ground. `steep` is
             * the local gradient, and 9 is the top sixth of it across six worlds, so the
             * outcrops fall along scarps and ridge lines instead of everywhere. */
            case B_MOUNTAIN:if (roll <  22 && steep >= 7) ob[AT(x,y)] = O_BOULDER;
                            else if (roll < 30) ob[AT(x,y)] = O_ORE;
                            else if (roll < 34) ob[AT(x,y)] = O_GOLD;
                            else if (roll < 36) ob[AT(x,y)] = O_GEM; break;
            case B_PEAK:    if (roll <  70 && steep >= 9) ob[AT(x,y)] = O_PEAKROCK; break;
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
/* biome[] -> layer[]: the cumulative band map the layered autotiler reads.
 *
 * Deep ocean is band -1 and sets NO bits, so it falls through to the background colour
 * and costs no layer at all. Everything else sets every bit up to its own band, which is
 * what lets layer L's fringe reveal the band below — and a neighbour in a lower band is
 * precisely a cell that lacks this bit, so the fringe shows the NEIGHBOUR's colour
 * without anybody looking a neighbour up.
 *
 * Rebuilt once a TICK rather than once a frame: terrain changes when the world changes,
 * and at 400 fps a per-frame sweep of 14336 cells would be five million writes a second
 * for nothing. */
void mb_bands_rebuild(void)
{
    /* biome -> band, indexed by B_*. -1 = the background (deep ocean). */
    static const int8_t BAND[B_N] = {
        -1,   /* B_NONE   */
        -1,   /* ocean    — the background */
         0,   /* sea      */
         0,   /* shallow  */
         1,   /* ice      */
         2,   /* beach    */
         3,   /* desert   */
         5,   /* savanna  */
         4,   /* grass    */
         4,   /* swamp    */
         6,   /* hill     */
         7,   /* mountain — rock underneath its sprite */
         7,   /* peak     */
         6,   /* tundra   */
         1,   /* snow     */
         6,   /* ash      — bare earth under its sprite, not stone */
         6,   /* scorched — dry under its sprite */
         6,   /* lava     — dry under its sprite */
         4,   /* acid     */
         6,   /* farm     */
         7,   /* rubble   */
         4,   /* meadow   */
         4,   /* forest   */
         7,   /* road     */
    };
    for (int i = 0; i < NC; i++) {
        int b = BAND[mb_w.biome[i]];
        mb_w.layer[i] = (b < 0) ? 0 : (uint8_t)((1u << (b + 1)) - 1u);
    }
}

void mb_grave_step(void)
{
    static uint32_t cur;
    for (int n = 0; n < NC / 64; n++) {
        uint32_t i = cur++ % NC;
        /* An ORPHAN BLUEPRINT — a plan square on ground no living village claims —
         * goes with them. Belt and braces beside clearing it at the village's death:
         * a marker that outlives its meaning is worse than no marker, and this sweep
         * is already walking the map. */
        if (mb_w.obj[i] == O_PLAN) {
            uint8_t cl = mb_w.claim[i];
            if (!cl || cl >= MAXV || !mb_v[cl].alive || mb_v[cl].plan_obj == 0)
                mb_w.obj[i] = O_NONE;
            continue;
        }
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
        case B_ASH:      if (roll < 130) mb_w.biome[i] = B_GRASS; continue;
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
