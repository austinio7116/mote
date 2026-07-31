/*
 * Motebox — the flux field: one mechanism, every disaster.
 *
 * flux[cell] packs kind:4 / intensity:4. A single rule pass per tick advances
 * all of it, so fire, lava, flood, acid and frost are five cases in one switch
 * rather than five systems. The terrain consequences land in biome[] and obj[],
 * which the engine autotiles directly — a burnt forest IS ash on the map, with
 * no extra draw path.
 *
 * DESIGN.md 10 planned a 4096-entry active-cell ring so a calm world cost
 * nothing. Measured, the whole-grid scan is 14336 loads of a byte that is
 * almost always zero — cheap enough at 8-64 ticks/s that the ring would have
 * bought a fraction of one percent of a core in exchange for a
 * duplicate-suppression structure and its bugs. It is not here.
 *
 * DOUBLE BUFFERED, deliberately: the pass reads flux[] and writes next[]. With
 * one buffer a fire races across the map in a single tick in the scan direction
 * and crawls in the other, because a cell it just lit is read again downstream.
 */
#include "mb.h"
#include <stdio.h>
#include <string.h>

static uint8_t *s_next;          /* what this tick writes; swapped in at the end */

/* --- the fuel model ----------------------------------------------------- */

/* How readily a cell's ground carries fire, 0-255. Vegetation burns, rock and
 * water do not, and a swamp is wet enough to resist. */
static uint8_t biome_fuel(uint8_t b)
{
    switch (b) {
    case B_SAVANNA: return 190;   /* dry grass: the fastest thing on the map */
    case B_FOREST:  return 230;
    case B_FARM:    return 200;
    case B_MEADOW:  return 170;
    case B_GRASS:   return 140;
    case B_SWAMP:   return  60;
    case B_TUNDRA:  return  50;
    default:        return   0;
    }
}

/* WHAT BURNS, and this list is most of the answer to "should fire burn everything
 * it touches". No: fire that consumes any cell it reaches is not a disaster, it is a
 * delete tool, and it is boring the second time. Fire is interesting exactly because
 * it is FUEL-GATED — it races through a forest, crawls across grass and stops dead at
 * a river, a road, a ploughed field or ground it has already burnt. That is the
 * mechanic Far Cry 2 built its savanna around and the one Minecraft, RimWorld and
 * WorldBox all use: per-material flammability plus a burn-out, so every fire has a
 * moving front and an end.
 *
 * BUT BUILDINGS HAD ZERO FUEL, so a village was fireproof. A god could drop fire on a
 * town of thatched houses and NOTHING HAPPENED — which is how the power audit found
 * it, casting FIRE on a village square to no effect at all. Now the settlement burns
 * on a materials gradient, which also gives the best story beat in the system: the
 * timber town goes up and the stone keep is still standing in the ashes. */
static uint8_t obj_fuel(uint8_t o)
{
    switch (o) {
    case O_DEAD:                return 255;   /* deadwood is tinder */
    case O_WOODCUT:             return 240;   /* a woodyard is a bonfire waiting */
    case O_TREE: case O_TREE2:  return 220;
    case O_FARM:                return 215;   /* a barn full of hay */
    case O_HOUSE1: case O_HOUSE2: case O_HOUSE3:
                                return 200;   /* timber and thatch */
    case O_DOCK:                return 185;   /* a wooden quay */
    case O_HALL1:               return 170;   /* still a big house at tier one */
    case O_BUSH: case O_REEDS:  return 150;
    case O_TOWER:               return 130;   /* its beacon is already lit */
    case O_TUFT: case O_FLOWER: return 110;
    case O_BARRACKS:            return  85;
    case O_HALL2:               return  55;   /* walled: it resists */
    case O_HALL3:               return  18;   /* the keep barely catches */
    /* stone does not burn: the temple's pillars, the mine head, the wall, and the
     * fire pit itself, which is the one thing in a village already on fire */
    default:                    return 0;
    }
}

/* What a cell becomes once its fire finally goes out. Vegetation leaves ash,
 * wet ground scorches. Converting on BURN-OUT rather than by per-tick chance is
 * what makes a firestorm a solid grey scar you can still read an hour later
 * instead of a half-eaten patch. */
static uint8_t burnt_biome(uint8_t b)
{
    switch (b) {
    case B_FOREST: case B_MEADOW: case B_GRASS:
    case B_SAVANNA: case B_FARM:  return B_ASH;
    case B_SWAMP:   case B_TUNDRA:return B_SCORCHED;
    default:                      return b;
    }
}

/* The acid ladder: each dose eats one step off the ground, ending in open sea.
 * Dissolving terrain all the way to water is WorldBox's acid, and it is the one
 * power that permanently shrinks a continent. */
static uint8_t acid_next(uint8_t b)
{
    switch (b) {
    case B_FOREST: case B_MEADOW: case B_SWAMP:  return B_GRASS;
    case B_GRASS:  case B_SAVANNA: case B_FARM:  return B_BEACH;
    case B_ASH:    case B_SCORCHED:              return B_BEACH;
    case B_SNOW:   case B_TUNDRA:                return B_RUBBLE;
    case B_PEAK:                                 return B_MOUNTAIN;
    case B_MOUNTAIN:                             return B_HILL;
    case B_HILL:   case B_RUBBLE: case B_DESERT: return B_BEACH;
    case B_BEACH:  case B_ICE:                   return B_SHALLOW;
    case B_SHALLOW:                              return B_SEA;
    default:                                     return b;
    }
}

/* RATE OF SPREAD IS NOT THE SAME THING AS FUEL LOAD, and conflating them was why the
 * "dry grass is the fastest thing on the map" note above described behaviour the code did
 * not have. One number drove both how fast fire jumped and how long a cell held its flame,
 * so shortening the burn to get a thin front also stopped savanna spreading at all.
 *
 * Fire science splits them and so does this: FINE, CURED fuel carries a front fastest —
 * grass and standing crop — while COARSE fuel burns hotter and longer but advances more
 * slowly. So a grass fire is a fast, thin, brief tongue running downwind, and a forest
 * fire is a slower, deeper, longer-lived band. Same model, two characters, and it comes
 * out of the fuel type rather than out of a special case.
 */
static uint8_t spread_rate(uint8_t b, uint8_t o)
{
    switch (b) {
    case B_SAVANNA: return 200;   /* cured grass: fine, dry, and it runs */
    case B_FARM:    return 175;   /* a standing crop does the same */
    case B_GRASS:   return 160;
    case B_MEADOW:  return 150;
    case B_FOREST:  return 110;   /* coarse: hotter and longer, but slower */
    case B_SWAMP:   return  45;
    case B_TUNDRA:  return  40;
    default: break;
    }
    /* bare ground around a building carries nothing, but the building itself catches */
    return (o && obj_fuel(o)) ? 90 : 0;
}

/* Fire intensity IS the fuel left to burn, so where it lands decides how long it
 * lives: a forest cell with a tree burns for 14 ticks, bare grass for 4. The
 * first model gave every cell a flat 10 and decayed it, which made a fire behave
 * identically in a rainforest and on a lawn — and, because a cast blob's edge
 * intensity fell off, made the FRONT the weakest part of the fire, so it always
 * guttered out after ten ticks instead of running. */
static int fuel_inten(int fuel)
{
    /* Halved: a forest cell used to hold its flame for fourteen weeks and, with the
     * front advancing a cell a week, everything within fourteen cells of the front was
     * alight at the same time — a disc. Seven weeks against a front that now creeps is
     * a band a couple of cells wide, which is what a wildfire looks like from above:
     * a bright moving edge with its own black wake behind it. */
    int i = fuel >> 6;
    return i < 3 ? 3 : (i > 8 ? 8 : i);   /* three weeks is the shortest useful flame */
}

/* --- wind ---------------------------------------------------------------
 * One global vector that turns slowly. Fire spreading downwind is the difference
 * between a disaster you watch and one you can predict, and it costs two bytes. */
static const int8_t WDX[8] = {  0,  1, 1, 1, 0, -1, -1, -1 };
static const int8_t WDY[8] = { -1, -1, 0, 1, 1,  1,  0, -1 };
static uint8_t s_wind_phase = 2;

void mb_flux_wind(int *dx, int *dy) { *dx = WDX[s_wind_phase]; *dy = WDY[s_wind_phase]; }
int  mb_flux_wind_phase(void)       { return s_wind_phase; }

static void wind_step(void)
{
    if ((mb_w.tick % 24) != 0) return;               /* about twice a year */
    int turn = (int)(mb_rand(0x711dea5u) & 3) - 1;   /* -1, 0, +1, 0 */
    s_wind_phase = (uint8_t)((s_wind_phase + 8 + turn) & 7);
}

/* --- flux accessors ----------------------------------------------------- */

static inline uint8_t fmake(int k, int i)
{
    if (i < 0) i = 0;
    if (i > 15) i = 15;
    return (uint8_t)((k << 4) | i);
}

/* Merge a dose into one cell of `buf`, keeping the stronger claim. Water beats
 * fire outright — that is how rain and flood put a firestorm out, and it has to
 * be a rule here rather than in each caller. */
static void add_to(uint8_t *buf, int x, int y, int kind, int inten)
{
    if (!mb_in(x, y) || inten <= 0 || kind == FX_NONE) return;
    uint8_t *f = &buf[AT(x, y)];
    int k = mb_fkind(*f), i = mb_fint(*f);
    if (kind == FX_WATER && k == FX_FIRE) { *f = fmake(FX_WATER, inten); return; }
    if (k == FX_WATER && kind == FX_FIRE) return;
    if (k == kind) { if (inten > i) *f = fmake(kind, inten); return; }
    if (k == FX_NONE || inten > i + 2) *f = fmake(kind, inten);
}

/* Powers cast between ticks, straight into the live array. */
void mb_flux_add(int x, int y, int kind, int inten) { add_to(mb_w.flux, x, y, kind, inten); }

/* Seed a disc — the shape every area power casts through. Intensity falls off
 * from the centre so a blob has a soft edge instead of a hard rim. */
void mb_flux_blob(int cx, int cy, int r, int kind, int inten)
{
    if (r <= 0) { mb_flux_add(cx, cy, kind, inten); return; }
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            mb_flux_add(x, y, kind, inten - (inten * d2) / (2 * r * r));
        }
}

/* BEAT OUT A FIRE. Returns how much intensity was actually removed, so the caller
 * knows whether the effort achieved anything — a villager who swings a wet sack at a
 * firestorm and makes no impression should not feel like one who saved a house.
 *
 * Water beats fire everywhere in this file, but a bucket is not a flood: dousing
 * subtracts intensity rather than replacing the cell with FX_WATER, so a big fire
 * takes many hands and many ticks and a small one goes out at once. That difference
 * is the whole drama of a town fighting for itself. */
int mb_flux_douse(int x, int y, int power)
{
    if (!mb_in(x, y)) return 0;
    uint8_t *f = &mb_w.flux[AT(x, y)];
    if (mb_fkind(*f) != FX_FIRE) return 0;
    int inten = mb_fint(*f);
    int cut = inten < power ? inten : power;
    inten -= cut;
    /* IT GOES OUT, and that is all it does. Leaving FX_WATER behind was tried, on the
     * reasoning that a bucket chain wets ground ahead of the front — but FX_WATER is
     * the FLOOD channel: it turns the tile into shallow water and deletes whatever is
     * standing on it, so a village fighting a fire drowned itself and demolished the
     * houses it was trying to save. It also chain-snuffed neighbouring cells, which
     * made four people enough to beat a ten-cell fire in five ticks.
     *
     * So a doused cell can be re-lit by its neighbours, and holding a fire means
     * having enough hands to out-pace the front rather than drawing a line once. The
     * numbers that make that a real contest are in the douse action, not here. */
    *f = inten > 0 ? fmake(FX_FIRE, inten) : 0;
    return cut;
}

/* Light a disc, each cell from its OWN fuel. Fire is the one flux whose strength
 * is a property of the ground rather than of the cast. */
void mb_flux_ignite(int cx, int cy, int r)
{
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if (!mb_in(x, y)) continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            int i = AT(x, y);
            int fuel = biome_fuel(mb_w.biome[i]) + obj_fuel(mb_w.obj[i]);
            if (fuel) mb_flux_add(x, y, FX_FIRE, fuel_inten(fuel));
        }
}

void mb_flux_init(void)
{
    s_next = (uint8_t *)g_api->alloc(NC);      /* once: the arena has no free */
    mb_flux_reset();
}

void mb_flux_reset(void);

/* --- the pass ----------------------------------------------------------- */

static void agent_step(void);

static const int8_t DX4[4] = { 1, -1, 0,  0 };
static const int8_t DY4[4] = { 0,  0, 1, -1 };

/* MOTEBOX_EVENT=<name> fires one world event on demand. Every one of these is rare by
 * design — a tsunami is one tick in 8192 — so waiting for one to look at it is not a test,
 * and "does the wave reach the shore" was unanswerable before this existed. */
void mb_flux_test_event(const char *name, uint32_t r);

/* --- THE TSUNAMI, which has to be a FRONT ------------------------------
 *
 * The first version laid a single column of flood cells — seventeen tall, one wide — at one
 * random x, walked east or west to find deep water, and let the ordinary flood rule spread
 * it. Three things were wrong with that and all three showed on screen:
 *
 *   IT WAS A STRIPE, not a wave. One cell wide is a leak.
 *   IT NEVER ARRIVED. Walking sideways to deep water usually put it well out to sea, and
 *     the flood rule only advances into cells at or below its own elevation, one per tick,
 *     losing a point of intensity each time — so it exhausted itself offshore.
 *   IT WAS OVER IN A SECOND. Intensity 12 decaying at 1 a tick is ten ticks total.
 *
 * A tsunami is a WALL OF WATER TRAVELLING IN A DIRECTION, so it needs to be stepped rather
 * than diffused. This keeps six bytes of state and advances a front: a wide arc, bowed at
 * the ends the way a real front refracts as it shoals, ragged along its length so it is not
 * a ruler, starting offshore so you watch it come in, and dying as it climbs — every column
 * stops when the ground gets too high for it, which is why a wave runs up a river valley
 * and stops at the dunes.
 *
 * It aims at a COAST NEAR A SETTLEMENT. A tsunami that lands on empty shore is a weather
 * report; the one that matters is the one you can see a town in front of.
 */
static struct {
    uint8_t on;          /* stepping */
    uint8_t x, y;        /* where it crosses the shore */
    int8_t  dx, dy;      /* inland */
    uint8_t half;        /* half-width of the front, in cells */
    uint8_t reach;       /* how far inland it climbs before it turns */
    int8_t  step;        /* current distance from the shore; negative = still at sea */
} s_wave;

static void wave_begin(uint32_t r)
{
    /* the shore nearest a living village, so the wave has something to threaten */
    int vx = -1, vy = -1, nv = 0;
    for (int v = 1; v < MAXV; v++) if (mb_v[v].alive) nv++;
    if (nv) {
        int want = (int)((r >> 3) % (uint32_t)nv), seen = 0;
        for (int v = 1; v < MAXV; v++)
            if (mb_v[v].alive && seen++ == want) { vx = mb_v[v].x; vy = mb_v[v].y; break; }
    }
    if (vx < 0) { vx = (int)((r >> 5) % MW); vy = (int)((r >> 13) % MH); }

    int bx = -1, by = -1, bd = 1 << 30;
    for (int y = 2; y < MH - 2; y++)
        for (int x = 2; x < MW - 2; x++) {
            if (!mb_land(mb_w.biome[AT(x, y)])) continue;
            int sea = 0;
            for (int k = 0; k < 4; k++)
                if (mb_water(mb_w.biome[AT(x + DX4[k], y + DY4[k])])) sea++;
            if (!sea) continue;                       /* not a shoreline cell */
            /* AND IT MUST FACE OPEN WATER. Any land cell beside any puddle is a
             * "shoreline", so the first version launched a tsunami out of an inland lake at
             * the top corner of the map. A wave needs a sea behind it: at least a third of
             * a five-cell neighbourhood under water. */
            int wide = 0;
            for (int oy = -5; oy <= 5; oy++)
                for (int ox = -5; ox <= 5; ox++) {
                    int nx = x + ox, ny = y + oy;
                    if (mb_in(nx, ny) && mb_water(mb_w.biome[AT(nx, ny)])) wide++;
                }
            /* Fifty-eight per cent of an eleven-cell neighbourhood, and eight cells clear
             * of the map edge. At a third the first version still launched out of a coastal
             * inlet in the top corner, so most of the front was off-map and what arrived was
             * a sliver. A tsunami needs open water behind it and room to be wide. */
            if (wide < 70) continue;
            if (x < 8 || x >= MW - 8 || y < 8 || y >= MH - 8) continue;
            int d = (x - vx) * (x - vx) + (y - vy) * (y - vy);
            if (d < bd) { bd = d; bx = x; by = y; }
        }
    if (bx < 0) {
        /* no open coast near that village: take the most exposed shore in the world instead,
         * so the event still happens rather than silently not happening */
        int best = 0;
        for (int y = 8; y < MH - 8; y++)
            for (int x = 8; x < MW - 8; x++) {
                if (!mb_land(mb_w.biome[AT(x, y)])) continue;
                int sea = 0;
                for (int k = 0; k < 4; k++)
                    if (mb_water(mb_w.biome[AT(x + DX4[k], y + DY4[k])])) sea++;
                if (!sea) continue;
                int wide = 0;
                for (int oy = -5; oy <= 5; oy++)
                    for (int ox = -5; ox <= 5; ox++)
                        if (mb_water(mb_w.biome[AT(x + ox, y + oy)])) wide++;
                if (wide > best) { best = wide; bx = x; by = y; }
            }
        if (bx < 0) return;
    }

    /* inland is away from the water: sum the offsets of the wet neighbours and flip it */
    int sx = 0, sy = 0;
    for (int k = 0; k < 4; k++)
        if (mb_water(mb_w.biome[AT(bx + DX4[k], by + DY4[k])])) { sx += DX4[k]; sy += DY4[k]; }
    if (!sx && !sy) return;
    /* one axis only: a front that advances diagonally would need a diagonal arc too, and
     * the cardinal case is the one that reads as a wave rolling in */
    if (sx * sx >= sy * sy) { s_wave.dx = (int8_t)(sx > 0 ? -1 : 1); s_wave.dy = 0; }
    else                    { s_wave.dx = 0; s_wave.dy = (int8_t)(sy > 0 ? -1 : 1); }

    s_wave.on = 1;
    s_wave.x = (uint8_t)bx; s_wave.y = (uint8_t)by;
    s_wave.half  = (uint8_t)(12 + ((r >> 21) & 7));      /* 25 to 39 cells across */
    s_wave.reach = (uint8_t)(9 + ((r >> 25) & 7));       /* and 9 to 16 cells inland */
    s_wave.step  = -4;                                  /* four cells out, still at sea */
    mb_chron_disaster("a tsunami", bx, by);
#if MOTE_HOST
    fprintf(stderr, "tsunami: shore %d,%d  inland %d,%d  half %d  reach %d\n",
            bx, by, s_wave.dx, s_wave.dy, s_wave.half, s_wave.reach);
#endif
}

static void wave_step(void)
{
    if (!s_wave.on) return;
    const int px = -s_wave.dy, py = s_wave.dx;           /* along the front */
    const int half = s_wave.half, st = s_wave.step;

    for (int t = -half; t <= half; t++) {
        /* THE ARC, AND IT MUST BE GENTLE. A front refracts as it shoals so the ends lag,
         * which is what makes a bowed line read as water rather than as a wipe — but the
         * first version used t*t/half, which lags the ENDS BY A FULL HALF-WIDTH. Only the
         * middle of the wave ever moved, so a thirty-cell front arrived as a ten-cell
         * blob. Four cells of lag at the tips is plenty to see. */
        int lag = (t * t * 4) / (half * half);
        unsigned h = (unsigned)(t * 2654435761u + s_wave.x * 40503u) ;
        h ^= h >> 13;
        int jit = (int)(h % 3u) - 1;
        int d = st - lag + jit;
        if (d < -6) continue;

        int x = (int)s_wave.x + s_wave.dx * d + px * t;
        int y = (int)s_wave.y + s_wave.dy * d + py * t;
        if (!mb_in(x, y)) continue;

        /* A COLUMN DIES WHEN THE GROUND BEATS IT. Height above the shore is what stops a
         * wave, so it runs up valleys and rivers and stops at the dunes — the shape of the
         * flood is the shape of the land, for free. */
        int rise = (int)mb_w.elev[AT(x, y)] - (int)mb_w.elev[AT(s_wave.x, s_wave.y)];
        if (d > 0 && rise > 3 + (int)s_wave.reach - d) continue;

        int inten = d <= 0 ? 12 : 12 - (d * 8) / (int)s_wave.reach;
        if (inten < 3) inten = 3;
        mb_flux_add(x, y, FX_WATER, inten);
    }

    s_wave.step++;
    if (s_wave.step > (int)s_wave.reach) { s_wave.on = 0; s_wave.step = 0; }
}



void mb_flux_step(void)
{
    uint8_t *flux = mb_w.flux, *next = s_next;
    uint8_t *bio = mb_w.biome, *ob = mb_w.obj, *elv = mb_w.elev;
    int wdx = WDX[s_wind_phase], wdy = WDY[s_wind_phase];

    wind_step();
    agent_step();          /* walking disasters seed flux before the pass reads it */
    wave_step();           /* and so does the tsunami's front */
    memset(next, 0, NC);

    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            const int i0 = AT(x, y);
            uint8_t f = flux[i0];
            if (!f) continue;
            int kind = mb_fkind(f), inten = mb_fint(f);
            uint32_t r = mb_rand((uint32_t)i0 * 2654435761u);
            uint8_t b = bio[i0], o = ob[i0];

            switch (kind) {

            case FX_FIRE:
                if (mb_water(b) || b == B_LAVA) { inten = 0; break; }
                if (!biome_fuel(b) && !obj_fuel(o)) {
                    inten -= 4;                       /* nothing left: starves */
                } else {
                    /* A BUILDING TAKES A WHILE TO GO. Everything flammable used to be
                     * destroyed at a flat 26-in-64 a tick, so the houses under a cast
                     * were gone within two ticks — before a single villager had even
                     * noticed, which is why firefighting halved the fire's spread and
                     * still saved exactly zero buildings. Vegetation keeps the old
                     * rate; a built thing burns at a fraction of it, so it stands long
                     * enough for the fight over it to have a result. */
                    if (o && obj_fuel(o)) {
                        int chance = mb_is_build(o) ? (obj_fuel(o) >> 5) : 26;
                        if ((int)(r & 63) < chance) ob[i0] = O_NONE;
                    }
                    inten -= 1;
                    for (int k = 0; k < 4; k++) {
                        int nx = x + DX4[k], ny = y + DY4[k];
                        if (!mb_in(nx, ny)) continue;
                        int ni = AT(nx, ny);
                        int nf = biome_fuel(bio[ni]) + obj_fuel(ob[ni]);
                        if (!nf) continue;
                        int sr = spread_rate(bio[ni], ob[ni]);
                        /* the chance depends on what is THERE and the wind, not on
                         * how much is left here — so a fire front does not weaken
                         * as it advances into fresh fuel */
                        int downwind = (DX4[k] == wdx && wdx) || (DY4[k] == wdy && wdy);
                        /* HALF THE OLD RATE, and the wind matters three times as much as
                         * the still air rather than twice. At the old (nf*4)>>3 a downwind
                         * forest neighbour caught with certainty every single tick, so the
                         * front advanced a full cell a week and the whole thing was over
                         * before you could look at it. A slower front is also the only way
                         * a burning band can be THIN: its thickness is the burn duration
                         * times the speed, so both had to come down together. */
                        /* Downwind is worth two and a half times still air, which is what
                         * gives a grass fire its long tongue: at 49% down and 20% across,
                         * savanna runs with the wind and barely widens, while forest at
                         * 27/11 spreads as a slow round front. */
                        int chance = (sr * (downwind ? 5 : 2)) >> 3;
                        if ((int)((r >> (k * 4 + 6)) & 255) < chance)
                            add_to(next, nx, ny, FX_FIRE, fuel_inten(nf));
                    }
                }
                if (inten <= 0) {
                    /* CONSUMED. The cell's biome went to ash but its OBJECT survived if
                     * the destroy roll above never fired — so a burnt cell still held a
                     * tree worth 220 fuel and its neighbours lit it again, and again.
                     * MOTEBOX_WILDFIRE measured the result: r_min 0 at tick 90, i.e. the
                     * origin of a forest fire was STILL BURNING ninety weeks later and
                     * four hundred cells were alight at once. That is why it read as a
                     * spreading disc instead of a front. Fire eats what it burns. */
                    bio[i0] = burnt_biome(b);
                    if (obj_fuel(o)) ob[i0] = O_NONE;
                }
                break;

            case FX_LAVA: {
                bio[i0] = B_LAVA; ob[i0] = O_NONE;
                int bx = -1, by = -1, low = elv[i0];
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX4[k], ny = y + DY4[k];
                    if (!mb_in(nx, ny)) continue;
                    int ni = AT(nx, ny);
                    if (elv[ni] < low) { low = elv[ni]; bx = nx; by = ny; }
                    if (biome_fuel(bio[ni]) + obj_fuel(ob[ni]))
                        add_to(next, nx, ny, FX_FIRE, 8);   /* it ignites its edges */
                }
                if (bx >= 0 && inten > 3) add_to(next, bx, by, FX_LAVA, inten - 1);
                inten -= 1;
                if (inten <= 0) {                            /* cooled: new rock */
                    bio[i0] = ((r >> 3) & 3) ? B_SCORCHED : B_RUBBLE;
                    if (elv[i0] < 250) elv[i0] = (uint8_t)(elv[i0] + 2);
                }
                break;
            }

            case FX_WATER:
                if (mb_land(b)) { bio[i0] = B_SHALLOW; ob[i0] = O_NONE; }
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX4[k], ny = y + DY4[k];
                    if (!mb_in(nx, ny)) continue;
                    int ni = AT(nx, ny);
                    if (mb_fkind(flux[ni]) == FX_FIRE) add_to(next, nx, ny, FX_WATER, 2);
                    if (inten > 2 && elv[ni] <= elv[i0] && mb_land(bio[ni]))
                        add_to(next, nx, ny, FX_WATER, inten - 1);
                }
                inten -= 1;
                /* recedes, leaving wet sand above sea level and sea below it */
                if (inten <= 0 && bio[i0] == B_SHALLOW)
                    bio[i0] = (elv[i0] >= mb_w.sea) ? B_BEACH : B_SEA;
                break;

            case FX_ACID:
                /* Decays at 1, not 2: at 2 a dose only ate two rungs off the
                 * ladder, so acid left a stain instead of a hole. One rung a tick
                 * for its whole life is what gets it down to open water, which is
                 * the point of the power. */
                if ((r & 7) < 5) bio[i0] = acid_next(b);
                ob[i0] = O_NONE;
                for (int k = 0; k < 4; k++)
                    if (inten > 4 && (int)((r >> (k * 3 + 9)) & 15) < 5)
                        add_to(next, x + DX4[k], y + DY4[k], FX_ACID, inten - 3);
                inten -= 1;
                break;

            case FX_FROST:
                if (b == B_SHALLOW || b == B_SEA) bio[i0] = B_ICE;
                else if (mb_land(b) && (r & 15) < 3) {
                    if (o) ob[i0] = O_NONE;                  /* the cold kills growth */
                    if (b == B_FOREST || b == B_MEADOW) bio[i0] = B_GRASS;
                    else if (b == B_GRASS || b == B_SAVANNA) bio[i0] = B_SNOW;
                }
                for (int k = 0; k < 4; k++)
                    if (inten > 3 && (int)((r >> (k * 3 + 4)) & 15) < 5)
                        add_to(next, x + DX4[k], y + DY4[k], FX_FROST, inten - 1);
                inten -= 1;
                break;

            default:
                inten = 0;
                break;
            }

            if (inten > 0) add_to(next, x, y, kind, inten);
        }
    }

    memcpy(flux, next, NC);
}

int mb_flux_count(void)
{
    int n = 0;
    for (int i = 0; i < NC; i++) if (mb_w.flux[i]) n++;
    return n;
}

/* --- agents: disasters that WALK ----------------------------------------
 * A tornado and a volcanic vent are not fields — they are a place that keeps
 * doing something for a while. Two bytes of kind and a countdown gives both, and
 * the same array will carry the tsunami front and the kaiju later.
 */
typedef struct {
    uint8_t  kind, alive;
    int16_t  x, y;
    int8_t   dx, dy;
    uint16_t life;        /* in ticks */
    int16_t  hp;          /* kaiju only: an army can bring one down */
} Agent;

#define NAGENT 6
static Agent s_ag[NAGENT];

/* A KAIJU IS FOUR CELLS, NOT ONE. The sprite is 2x2 and stands with its feet on the agent's
 * cell, so its body covers (x..x+1, y-1..y) — and every effect used to radiate from the single
 * anchor cell, which put a dragon's fire under its left foot and a golem's rubble behind its
 * heel. Effects walk this footprint, so what the beast does comes out of where the beast IS. */
static const int8_t BODY_X[4] = { 0, 1, 0, 1 };
static const int8_t BODY_Y[4] = { -1, -1, 0, 0 };

#define KAIJU_LIFE   900     /* ticks; see mb_agent_spawn */
#define KAIJU_HP    6000     /* and it takes a siege to bring one down */
#define KAIJU_SHAKE   28     /* and it only shakes for the first few seconds of them */

void mb_agent_spawn(int kind, int x, int y)
{
    int slot = -1;
    for (int i = 0; i < NAGENT; i++) if (!s_ag[i].alive) { slot = i; break; }
    if (slot < 0) {            /* full: replace the one with least life left */
        slot = 0;
        for (int i = 1; i < NAGENT; i++) if (s_ag[i].life < s_ag[slot].life) slot = i;
    }
    Agent *a = &s_ag[slot];
    uint32_t r = mb_rand((uint32_t)(x * 71 + y * 37));
    a->kind = (uint8_t)kind; a->alive = 1;
    a->x = (int16_t)x; a->y = (int16_t)y;
    a->dx = (int8_t)((int)(r & 3) - 1);
    a->dy = (int8_t)((int)((r >> 2) & 3) - 1);
    if (!a->dx && !a->dy) a->dx = 1;
    a->life = (kind == AG_TORNADO) ? 90 : (kind == AG_VENT ? 40 :
              (kind == AG_MAW ? 60000 : 900));      /* the Maw never stops */
    /* SIX THOUSAND, not nine hundred. Once villagers would actually go and fight a monster,
     * nine hundred turned out to be one second of a mob: a dozen spears reach it at twenty-five
     * damage a blow, so the titan fell in EIGHT TICKS and the golem in six. A kaiju should
     * rampage for a while and then be brought down — the story is the siege, not the swat. */
    a->hp = (kind >= AG_KAIJU0 && kind < AG_MAW) ? 6000 : 0;
    /* NAME IT. Every one of the seven announced itself as "a titan walks", so the log could
     * not tell you which of them was loose — and they no longer do remotely the same thing. */
    if (kind >= AG_KAIJU0 && kind < AG_MAW) {
        static const char *const WALKS[] = {
            "a titan walks", "a medusa walks", "Death walks", "a DRAGON wakes",
            "a golem walks", "the Eye opens", "an angel descends",
        };
        mb_chron_disaster(WALKS[kind - AG_KAIJU0], x, y);
    }
    if (kind == AG_MAW) mb_chron_disaster("the Maw opens", x, y);
}

/* An army fighting a kaiju: units call this when they are adjacent to one, which
 * is how a kingdom can actually win. Returns 1 if the blow killed it. */
int mb_agent_hurt(int x, int y, int dmg)
{
    for (int i = 0; i < NAGENT; i++) {
        Agent *a = &s_ag[i];
        if (!a->alive || a->kind < AG_KAIJU0 || a->kind >= AG_MAW) continue;
        int dx = a->x - x, dy = a->y - y;
        if (dx * dx + dy * dy > 6) continue;
        a->hp -= (int16_t)dmg;
        if (a->hp <= 0) {
            a->alive = 0;
            mb_chron_disaster("the titan falls", a->x, a->y);
            mb_fx_impact((float)a->x, (float)a->y, FXE_VOID, 1.4f);
            return 1;
        }
        return 0;
    }
    return 0;
}

int mb_agent_hp(int i) { return (i >= 0 && i < NAGENT) ? s_ag[i].hp : 0; }

int mb_agent_count(void)
{
    int n = 0;
    for (int i = 0; i < NAGENT; i++) if (s_ag[i].alive) n++;
    return n;
}

/* Where the agents are, so the renderer can draw them and `seek` can find them. */
int mb_agent_get(int i, int *x, int *y, int *kind)
{
    if (i < 0 || i >= NAGENT || !s_ag[i].alive) return 0;
    *x = s_ag[i].x; *y = s_ag[i].y; *kind = s_ag[i].kind;
    return 1;
}
int mb_agent_max(void) { return NAGENT; }

static void agent_step(void)
{
    int wdx, wdy;
    mb_flux_wind(&wdx, &wdy);

    for (int i = 0; i < NAGENT; i++) {
        Agent *a = &s_ag[i];
        if (!a->alive) continue;
        uint32_t r = mb_rand((uint32_t)(i * 9176u + 13u));

        if (a->kind == AG_TORNADO) {
            /* drifts downwind with a wobble, so its track curves like a real one
             * instead of ruling a straight line across the map */
            if ((r & 3) == 0) { a->dx = (int8_t)wdx; a->dy = (int8_t)wdy; }
            else if ((r & 7) == 1) {
                a->dx = (int8_t)((int)((r >> 4) & 3) - 1);
                a->dy = (int8_t)((int)((r >> 6) & 3) - 1);
            }
            a->x = (int16_t)(a->x + a->dx);
            a->y = (int16_t)(a->y + a->dy);
            /* scour a 3-wide swathe to bare rock and strip everything on it */
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int x = a->x + dx, y = a->y + dy;
                    if (!mb_in(x, y)) continue;
                    int c = AT(x, y);
                    if (!mb_land(mb_w.biome[c])) continue;
                    mb_w.obj[c] = O_NONE;
                    if ((int)((r >> (dx + dy + 4)) & 7) < 5) mb_w.biome[c] = B_RUBBLE;
                }
            mb_fx_burst((float)a->x, (float)a->y, 3, PK_GUST, FXE_ASH, 4.0f, 0.5f);
            mb_fx_shake(1.2f);
        } else if (a->kind >= AG_KAIJU0 && a->kind < AG_MAW) {
            /* A KAIJU walks toward the nearest village and wrecks it. It is the
             * crowd-pleaser precisely because it has a GOAL: a monster that
             * wandered would read as weather, and this reads as an attack. */
            int tx = a->x, ty = a->y, bestd = 1 << 30;
            for (int v = 1; v < MAXV; v++) {
                if (!mb_v[v].alive) continue;
                int dx = mb_v[v].x - a->x, dy = mb_v[v].y - a->y;
                int d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; tx = mb_v[v].x; ty = mb_v[v].y; }
            }
            /* THE GOLEM IS SLOW. It is made of rock; a thing made of rock should look like
             * it weighs something, and a monster you can outrun is a different threat from
             * one you cannot. */
            int slow = (a->kind == AG_GOLEM);
            if ((r & 3) != 3 && !(slow && (r & 4))) {   /* three ticks in four it moves */
                int sx = (tx > a->x ? 1 : (tx < a->x ? -1 : 0));
                int sy = (ty > a->y ? 1 : (ty < a->y ? -1 : 0));
                a->x = (int16_t)(a->x + sx);
                a->y = (int16_t)(a->y + sy);
                /* remember the heading: the phoenix breathes along it */
                if (sx || sy) { a->dx = (int8_t)sx; a->dy = (int8_t)sy; }
            }

            /* --- WHAT EACH BEAST ACTUALLY DOES ---------------------------------------
             *
             * Six of the seven used to do the SAME thing: hurt everything within two cells
             * and flatten buildings within one. So a medusa, a reaper, a golem, a great eye
             * and a skull titan were one monster wearing five sprites, and the only way to
             * tell which you had summoned was to read the icon you had just clicked. A
             * bestiary needs verbs, not skins — and each of these is a verb you can see from
             * God's Eye without being told which beast is loose. */
            switch (a->kind) {

            case AG_ANGEL:
                /* the one that mends: it lifts curses and madness and heals the rest */
                for (int f = 0; f < 4; f++) {
                    int bx = a->x + BODY_X[f], by = a->y + BODY_Y[f];
                    mb_unit_area(bx, by, 3, UAP_HEAL, 0);
                    mb_unit_area(bx, by, 3, UAP_UNTRAIT, TR_CURSED | TR_MADNESS);
                }
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 4,
                            PK_STAR, FXE_HOLY, 2.0f, 0.8f);
                break;

            case AG_DRAGON: {
                /* IT BREATHES, and this is the one people summon a god for.
                 *
                 * The breath leaves the MOUTH — the front-top of a 2x2 body, one cell along
                 * the heading — not the anchor cell under its feet, which is where the old
                 * ring of ignition came from and read as a bird standing in a campfire.
                 * Seven cells deep, widening from one to five, hottest at the throat and
                 * guttering out at the tip, with the flame drawn as three sparking jets along
                 * its length so the shape reads before the ground catches. Everything in the
                 * cone burns; buildings are left standing to burn down on their own, which is
                 * slower and worse than being flattened. */
                int hx = a->dx ? a->dx : 1, hy = a->dy;
                int mx = a->x + (hx > 0 ? 1 : 0) + hx, my = a->y - 1 + hy;

                /* AND IT ONLY BREATHES AT SOMETHING. The breath fired on every agent tick
                 * wherever the dragon happened to be looking, so it crossed empty moorland
                 * laying a permanent burning furrow behind it and arrived at the town it was
                 * summoned to destroy having already set fire to half a continent. A dragon
                 * breathing constantly also reads as a machine rather than an animal.
                 *
                 * So it looks first: a person, an animal or a building anywhere in the cone it
                 * is about to fill. Nothing there, no fire — it just flies on, which is both
                 * cheaper and much more frightening to watch. */
                int prey = 0;
                for (int step = 0; step < 7 && !prey; step++) {
                    int spread = step / 2;  if (spread > 2) spread = 2;
                    for (int off = -spread; off <= spread && !prey; off++) {
                        int bx = mx + hx * step - hy * off;
                        int by = my + hy * step + hx * off;
                        if (!mb_in(bx, by)) continue;
                        if (mb_is_build(mb_w.obj[AT(bx, by)])) prey = 1;
                    }
                }
                for (int i = 0; i < mb_nu && !prey; i++) {
                    if (!mb_u[i].alive) continue;
                    int dx = (mb_u[i].x >> 4) - mx, dy = (mb_u[i].y >> 4) - my;
                    /* in front of it, not behind: the dot product with the heading */
                    if (dx * hx + dy * hy < 0) continue;
                    if (dx * dx + dy * dy <= 7 * 7) prey = 1;
                }
#if MOTE_HOST
                {   extern uint32_t mb_breath_fired, mb_breath_held;
                    if (prey) mb_breath_fired++; else mb_breath_held++; }
#endif
                if (!prey) break;

                for (int step = 0; step < 7; step++) {
                    int spread = step / 2;                     /* 0,0,1,1,2,2,3 -> a cone */
                    if (spread > 2) spread = 2;
                    for (int off = -spread; off <= spread; off++) {
                        int bx = mx + hx * step - hy * off;
                        int by = my + hy * step + hx * off;
                        if (!mb_in(bx, by)) continue;
                        if (!mb_land(mb_w.biome[AT(bx, by)])) continue;
                        add_to(mb_w.flux, bx, by, FX_FIRE, (uint8_t)(15 - step));
                    }
                    if ((step & 2) == 0)
                        mb_fx_burst((float)(mx + hx * step), (float)(my + hy * step),
                                    1 + spread, PK_SPARK, FXE_FIRE, 4.0f - step * 0.4f, 0.7f);
                    mb_unit_area(mx + hx * step, my + hy * step, spread,
                                 UAP_HURT, 55 - step * 5);
                }
                break;
            }

            case AG_MEDUSA:
                /* IT PETRIFIES. Everything that meets its eye becomes stone, so it kills
                 * without breaking anything and leaves a field of boulders behind it —
                 * a track you can still read across the map a century later. */
                for (int f = 0; f < 4; f++) {
                    int cx0 = a->x + BODY_X[f], cy0 = a->y + BODY_Y[f];
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int bx = cx0 + dx, by = cy0 + dy;
                            if (!mb_in(bx, by) || !mb_land(mb_w.biome[AT(bx, by)])) continue;
                            if (mb_w.obj[AT(bx, by)]) continue;
                            if ((int)((r >> ((dx + dy + f + 5) & 15)) & 15) < 3)
                                mb_w.obj[AT(bx, by)] = O_BOULDER;   /* the statues it makes */
                        }
                    mb_unit_area(cx0, cy0, 2, UAP_KILL, CAUSE_DISASTER);
                }
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 3,
                            PK_RING, FXE_FROST, 1.2f, 0.8f);
                break;

            case AG_REAPER:
                /* IT REAPS, and takes nothing else. No building falls and no ground is
                 * spoiled — the town is simply emptied, and everyone it takes leaves a GRAVE,
                 * which is what the undead rising feeds on. Two disasters that chain into each
                 * other are worth more than two that do not. */
                for (int f = 0; f < 4; f++) {
                    int cx0 = a->x + BODY_X[f], cy0 = a->y + BODY_Y[f];
                    for (int dy = -3; dy <= 3; dy++)
                        for (int dx = -3; dx <= 3; dx++) {
                            if (dx * dx + dy * dy > 9) continue;
                            int bx = cx0 + dx, by = cy0 + dy;
                            if (!mb_in(bx, by) || !mb_land(mb_w.biome[AT(bx, by)])) continue;
                            if (mb_w.obj[AT(bx, by)]) continue;
                            if ((int)((r >> ((dx + dy + f + 6) & 15)) & 31) < 4)
                                mb_w.obj[AT(bx, by)] = O_GRAVE;
                        }
                    mb_unit_area(cx0, cy0, 3, UAP_KILL, CAUSE_DISASTER);
                }
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 4,
                            PK_SMOKE, FXE_VOID, 1.0f, 1.0f);
                break;

            case AG_EYE:
                /* IT LOOKS AT YOU. The great eye kills nobody: it curses and enrages over a
                 * wide circle and lets the town do the rest, which is the most frightening of
                 * the seven precisely because nothing visibly attacks. The mad do not pray
                 * either, so it drains the Faith of whoever called it. */
                for (int f = 0; f < 4; f++) {
                    int cx0 = a->x + BODY_X[f], cy0 = a->y + BODY_Y[f];
                    mb_unit_area(cx0, cy0, 5, UAP_TRAIT, TR_MADNESS | TR_CURSED);
                    mb_unit_area(cx0, cy0, 5, UAP_RAGE, 0);
                }
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 6,
                            PK_RING, FXE_VOID, 0.8f, 1.0f);
                break;

            case AG_GOLEM:
                /* IT CRUSHES. Slow, and it does not knock a building down so much as bury it:
                 * everything under its four cells goes to rubble, the ground with it, and the
                 * rubble stays. A golem leaves a road of ruin exactly its own width. */
                for (int f = 0; f < 4; f++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int bx = a->x + BODY_X[f] + dx, by = a->y + BODY_Y[f] + dy;
                            if (!mb_in(bx, by) || !mb_land(mb_w.biome[AT(bx, by)])) continue;
                            mb_w.obj[AT(bx, by)] = O_NONE;
                            mb_w.biome[AT(bx, by)] = B_RUBBLE;
                        }
                mb_unit_area(a->x, a->y, 2, UAP_HURT, 70);
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 3,
                            PK_SMOKE, FXE_ASH, 1.4f, 1.0f);
                break;

            default:
                /* AG_TITAN and anything new: the wrecker. Its verb is DEMOLITION — it takes
                 * buildings down over a wider reach than anything else and leaves the people
                 * to flee, which is why it is the one you summon at a city. */
                for (int f = 0; f < 4; f++) {
                    int cx0 = a->x + BODY_X[f], cy0 = a->y + BODY_Y[f];
                    mb_unit_area(cx0, cy0, 2, UAP_HURT, 60);
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int bx = cx0 + dx, by = cy0 + dy;
                            if (!mb_in(bx, by)) continue;
                            if (mb_is_build(mb_w.obj[AT(bx, by)])) {
                                mb_w.obj[AT(bx, by)] = O_NONE;
                                mb_w.biome[AT(bx, by)] = B_RUBBLE;
                            }
                        }
                }
                mb_fx_burst((float)a->x + 0.5f, (float)a->y - 0.5f, 3,
                            PK_SMOKE, FXE_VOID, 1.5f, 0.9f);
                break;
            }
            /* THE GROUND ONLY SHAKES WHILE IT ARRIVES. This was an unconditional shake every
             * tick of a nine-hundred-tick life, so a medusa that nobody killed rattled the
             * screen for two solid minutes and the shake stopped meaning anything. A few
             * seconds of it is an entrance; the rest is a headache. */
            if (a->life > KAIJU_LIFE - KAIJU_SHAKE)
                mb_fx_shake(a->kind == AG_GOLEM ? 2.4f : 1.6f);

        } else if (a->kind == AG_MAW) {
            /* THE MAW: a void that eats the world and never stops. It grows
             * outward one ring at a time, and nothing puts it back. */
            int grow = 3 + (int)(r & 3);
            for (int t = 0; t < grow; t++) {
                uint32_t q = mb_rand((uint32_t)(i * 71u + t * 13u));
                int rad = 1 + (int)(q % 3);
                int ang = (int)((q >> 4) & 7);
                static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
                static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
                int mx = a->x + DX[ang] * rad, my = a->y + DY[ang] * rad;
                if (!mb_in(mx, my)) continue;
                mb_w.biome[AT(mx, my)] = B_OCEAN;    /* the sea takes it back */
                mb_w.obj[AT(mx, my)] = O_NONE;
                mb_w.claim[AT(mx, my)] = 0;
                mb_w.elev[AT(mx, my)] = 0;
            }
            mb_unit_area(a->x, a->y, 2, UAP_KILL, CAUSE_DISASTER);
            /* AND THINGS COME OUT OF IT. The Maw is a hole into somewhere else that eats the
             * world and never stops, and until now the only thing that ever emerged was more
             * ocean. A demon every so often is what makes it a gate rather than a drain. */
            if ((r & 63) == 0) {
                int sx = a->x + (int)((r >> 8) & 7) - 3, sy = a->y + (int)((r >> 11) & 7) - 3;
                if (mb_in(sx, sy) && mb_land(mb_w.biome[AT(sx, sy)]))
                    mb_unit_spawn(SP_DEMON, sx, sy);
            }
            /* it drifts, so it does not simply drill one hole */
            if ((r & 7) == 0) { a->x = (int16_t)(a->x + a->dx); a->y = (int16_t)(a->y + a->dy); }
            mb_fx_burst((float)a->x, (float)a->y, 2, PK_RING, FXE_VOID, 0.6f, 1.0f);
        } else {                                    /* AG_VENT */
            /* a volcano keeps erupting: lava every tick, so it builds a cone and
             * the flow finds its own drainage */
            mb_flux_add(a->x, a->y, FX_LAVA, 14);
            if ((r & 1)) mb_flux_add(a->x + (int)((r >> 1) & 1) - 1,
                                     a->y + (int)((r >> 2) & 1) - 1, FX_LAVA, 12);
            if (mb_in(a->x, a->y) && mb_w.elev[AT(a->x, a->y)] < 250)
                mb_w.elev[AT(a->x, a->y)] += 1;
            mb_fx_burst((float)a->x, (float)a->y, 2, PK_SPARK, FXE_FIRE, 3.0f, 0.7f);
        }

        if (!mb_in(a->x, a->y) || --a->life == 0) a->alive = 0;
    }
}

/* --- natural disasters --------------------------------------------------
 * WorldBox gates what can fire naturally on the world's own state: population,
 * city count and the age. Same philosophy here, and the age is most of it — the
 * Age of Sun is a tinderbox, the Age of Ice freezes the sea, the Age of Ash
 * falls on everyone. A world with nobody in it gets left alone, because a
 * disaster nothing witnesses is just noise.
 */
void mb_flux_natural(void)
{
    if (!mb_law(LAW_DISASTER)) return;
    if (mb_pop_civ() < 6) return;                     /* nobody to witness it */
    uint32_t r = mb_rand(0x4a7fu);

    int dry = mb_age_dryness(), cold = mb_age_cold();

    /* HEATWAVE / spontaneous fire: dry ground ignites on its own */
    if (dry > 100 && (r & 255) < (uint32_t)(dry >> 4)) {
        int x = (int)((r >> 8) % MW), y = (int)((r >> 18) % MH);
        mb_flux_ignite(x, y, 1);
        if ((r & 7) == 0) mb_chron_disaster("a heatwave", x, y);
    }

    /* BLIZZARD: frost at the latitudes the climate already made cold */
    if (cold > 100 && (r & 255) < (uint32_t)(cold >> 4)) {
        int x = (int)((r >> 9) % MW);
        int y = (r & 0x10000u) ? (int)((r >> 19) % 18) : MH - 1 - (int)((r >> 19) % 18);
        mb_flux_blob(x, y, 2, FX_FROST, 7);
        if ((r & 15) == 0) mb_chron_disaster("a blizzard", x, y);
    }

    /* ASHFALL: the Age of Ash on everyone, a little at a time. Deliberately weak
     * — at 3 damage in a radius of 4, three times every four ticks, it wiped out
     * every living thing on the map inside thirty years and the 300-year curve
     * ended with pop 0, wild 0. An age should press on a world, not end it. */
    if (mb_age_id() == AGE_ASH && (mb_w.tick % 16) == 0) {
        uint32_t q = mb_rand(0x5a1fu);
        int x = (int)(q % MW), y = (int)((q >> 10) % MH);
        if (mb_land(mb_w.biome[AT(x, y)])) mb_unit_area(x, y, 3, UAP_HURT, 2);
    }

    /* PLAGUE: it starts somewhere, once in a long while, in the densest place */
    if (mb_law(LAW_PLAGUE) && (r & 0xFFFu) == 0) {
        int best = 0, bestp = 8;
        for (int v = 1; v < MAXV; v++)
            if (mb_v[v].alive && mb_v[v].pop > bestp) { bestp = mb_v[v].pop; best = v; }
        if (best) {
            mb_unit_area(mb_v[best].x, mb_v[best].y, 3, UAP_TRAIT, TR_PLAGUE | TR_CONTAGIOUS);
            mb_chron_disaster("a plague", mb_v[best].x, mb_v[best].y);
        }
    }

    /* TSUNAMI: START one, if the sea is minded. The wave itself is stepped by
     * wave_step() below — this only decides that one begins, and where. */
    if ((r & 0x1FFFu) == 0 && !s_wave.on) wave_begin(r);

    /* SINKHOLE: the ground simply goes. Small, permanent, and it takes whatever was
     * standing on it — the cheapest disaster in the game and one of the nastiest,
     * because there is no front to see coming. */
    if ((r & 0xFFFu) == 1) {
        int cx = (int)((r >> 12) % MW), cy = (int)((r >> 21) % MH);
        if (mb_in(cx, cy) && mb_land(mb_w.biome[AT(cx, cy)])) {
            int rad = 1 + (int)((r >> 6) & 1);
            for (int y = cy - rad; y <= cy + rad; y++)
                for (int x = cx - rad; x <= cx + rad; x++) {
                    if (!mb_in(x, y)) continue;
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > rad * rad) continue;
                    mb_w.biome[AT(x, y)] = B_RUBBLE;
                    mb_w.obj[AT(x, y)] = O_NONE;
                    mb_w.elev[AT(x, y)] = (uint8_t)(mb_w.elev[AT(x, y)] > 30
                                                    ? mb_w.elev[AT(x, y)] - 30 : 0);
                }
            mb_unit_area(cx, cy, rad, UAP_KILL, CAUSE_DISASTER);
            mb_chron_disaster("the ground opens", cx, cy);
        }
    }

    /* THE UNDEAD: graveyards left by a war or a famine stand up on their own */
    if (mb_law(LAW_MONSTERS) && (r & 0x7FFu) == 0) {
        int gx = (int)((r >> 12) % MW), gy = (int)((r >> 22) % MH);
        if (mb_unit_raise_dead(gx, gy, 5) > 0)
            mb_chron_disaster("the dead rise", gx, gy);
    }
}

void mb_flux_reset(void)
{
    memset(mb_w.flux, 0, NC);
    for (int i = 0; i < NAGENT; i++) s_ag[i].alive = 0;
    s_wind_phase = 2;
}

/* --- ON-DEMAND WORLD EVENTS (test hook) ---------------------------------
 * The bodies live inside mb_flux_step()'s random-event block, which cannot be called for
 * one event alone; the two that keep their own state (the tsunami) or are one-shot writes
 * (the sinkhole) are reachable here. Anything added to the disaster block should get an
 * entry, because an event nobody can trigger is an event nobody has watched. */
void mb_flux_test_event(const char *name, uint32_t r)
{
    if (!name) return;
    if (!strcmp(name, "tsunami")) { s_wave.on = 0; wave_begin(r); return; }
    if (!strcmp(name, "sinkhole")) {
        int cx = (int)(r % MW), cy = (int)((r >> 9) % MH);
        for (int t = 0; t < 400; t++) {                /* land, and near a village if we can */
            cx = (int)((r >> (t & 15)) % MW); cy = (int)((r >> ((t + 7) & 15)) % MH);
            if (mb_in(cx, cy) && mb_land(mb_w.biome[AT(cx, cy)])) break;
        }
        int rad = 2;
        for (int y = cy - rad; y <= cy + rad; y++)
            for (int x = cx - rad; x <= cx + rad; x++) {
                if (!mb_in(x, y)) continue;
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > rad * rad) continue;
                mb_w.biome[AT(x, y)] = B_RUBBLE; mb_w.obj[AT(x, y)] = O_NONE;
                mb_w.elev[AT(x, y)] = (uint8_t)(mb_w.elev[AT(x, y)] > 30
                                                ? mb_w.elev[AT(x, y)] - 30 : 0);
            }
        mb_unit_area(cx, cy, rad, UAP_KILL, CAUSE_DISASTER);
        mb_chron_disaster("the ground opens", cx, cy);
        return;
    }
}
/* --- THE BOMB ------------------------------------------------------------
 *
 * The tech tree has to end somewhere that MATTERS, and the reason to put a nuclear strike
 * there rather than another building is that it is the one thing a kingdom can do that
 * permanently changes the world rather than its own corner of it.
 *
 * Four things happen, in the order you would see them:
 *   THE FLASH, as particles, bright and brief.
 *   THE CRATER, dug rather than painted: elevation gone, everything standing gone, and the
 *     ground turned to scorched rock and rubble. It is permanent — nothing in the healing
 *     rules brings scorched rock back to grass — so a world that ends this way carries the
 *     mark for the rest of its history, which is the point.
 *   THE FIRESTORM, a ring of fire outside the crater, which then does what fire does.
 *   THE FALLOUT, as lingering flux that sickens and kills anything that walks through it.
 *
 * It is deliberately far bigger than any god power. A meteor is a disaster; this is a
 * decision somebody made.
 */
/* --- A BLAST IS A FRONT, NOT AN EVENT ------------------------------------
 *
 * Every bit of this — the crater, the firestorm twelve cells out, the dead — was applied on the
 * single tick of the launch, while the cloud above it took three seconds to climb. So the fire
 * had spread off the screen before the column had left the ground: the two halves of the same
 * explosion ran on different clocks and the order was backwards.
 *
 * The damage now travels. One cell of radius per tick from ground zero, which at eight ticks a
 * second reaches the edge of the firestorm in about a second and a half — so the sequence a
 * player sees is the flash, then the ground going dark under the fireball, then the ring of fire
 * running outward, then the column, then the cap spreading. People die as the wave reaches them
 * rather than all at once, which is also how the crowd scatters outward ahead of it.
 *
 * On the SIMULATION's clock, not the renderer's: mb_flux_step runs in the headless fast-forward
 * too, and a strike in a fast-forwarded century has to complete like any other.
 */
#define NBLAST 2
#define BLAST_RAD  7                        /* the crater */
#define BLAST_BURN 12                       /* and the firestorm around it */
static struct { uint8_t on, x, y, r; } s_blast[NBLAST];
#if MOTE_HOST
uint32_t mb_dosed, mb_breath_fired, mb_breath_held;
#endif

static void blast_ring(int bx, int by, int r)
{
    /* the cells whose distance from ground zero has just been reached */
    for (int y = by - r; y <= by + r; y++)
        for (int x = bx - r; x <= bx + r; x++) {
            if (!mb_in(x, y)) continue;
            int d2 = (x - bx) * (x - bx) + (y - by) * (y - by);
            if (d2 > r * r || d2 <= (r - 1) * (r - 1)) continue;      /* this ring only */
            int i = AT(x, y);
            if (r <= BLAST_RAD) {
                if (mb_land(mb_w.biome[i])) {
                    mb_w.biome[i] = (r <= BLAST_RAD / 2) ? B_RUBBLE : B_SCORCHED;
                    mb_w.obj[i] = O_NONE;
                    mb_w.road[i] = 0;
                    mb_w.elev[i] = (uint8_t)(mb_w.elev[i] > 24 ? mb_w.elev[i] - 24 : 0);
                }
                /* NO ACID HERE. The first version used FX_ACID as "fallout", and acid dissolves
                 * terrain a rung at a time all the way down to open sea — so within a minute the
                 * crater had filled with BRIGHT BLUE WATER and a nuclear strike looked like
                 * somebody had dug a lake. The scar has to be dry. */
            } else if ((mb_rand((uint32_t)i * 2654435761u) & 3) != 0) {
                mb_flux_ignite(x, y, 0);
            }
        }
    /* and the people the front has just reached */
    if (r <= BLAST_RAD + 2) mb_unit_area(bx, by, r, UAP_KILL, CAUSE_DISASTER);
}

void mb_blast_step(void)
{
    /* TWO CELLS A TICK, not one. At one the ground took a second and a half to finish while the
     * column was still climbing, which made the blast look like a slow stain spreading rather
     * than a shock — the cloud's timing is right and the ground's was half its speed. Both rings
     * are applied so no cell is stepped over. */
    for (int i = 0; i < NBLAST; i++) {
        if (!s_blast[i].on) continue;
        for (int step = 0; step < 2 && s_blast[i].on; step++) {
            s_blast[i].r++;
            blast_ring(s_blast[i].x, s_blast[i].y, s_blast[i].r);
            if (s_blast[i].r >= BLAST_BURN) s_blast[i].on = 0;
        }
    }
}

void mb_blast_clear(void) { for (int i = 0; i < NBLAST; i++) s_blast[i].on = 0; }

void mb_nuke_strike(int from_k, int cx, int cy)
{
    if (!mb_in(cx, cy)) return;

    mb_snd(SND_BOOM);
    mb_fx_impact((float)cx, (float)cy, FXE_FIRE, 3.0f);
    mb_fx_nuke(cx, cy);                    /* and the shape everyone recognises */
    for (int i = 0; i < 24; i++) {
        float a = (float)i * 0.2617f;
        mb_fx_spawn((float)cx + (float)i * 0.05f, (float)cy - 2.0f - (float)i * 0.15f,
                    PK_RING, FXE_FIRE, a, 0.9f);
    }

    /* ground zero goes at once; everything further out waits for the front */
    int slot = 0;
    for (int i = 0; i < NBLAST; i++) if (!s_blast[i].on) { slot = i; break; }
    s_blast[slot].on = 1; s_blast[slot].x = (uint8_t)cx; s_blast[slot].y = (uint8_t)cy;
    s_blast[slot].r = 0;
    blast_ring(cx, cy, 1);

    /* RADIATION SICKNESS, on the survivors just outside the crater. There is no fallout flux
     * kind and adding one would touch six tables, but the plague machinery already models
     * exactly this: an illness that kills slowly, spreads to whoever tends the sick, and
     * leaves the immune behind. So the ring outside the blast is where the world learns what
     * the bomb really costs, weeks after the flash. */
    /* AND IT IS FOR THE SURVIVORS. Applying the dose out to the firestorm's edge wasted almost
     * all of it: everyone within nine cells is killed by the front as it passes and everything
     * from there to twelve is burning. The people who matter are the ring OUTSIDE — far enough
     * to live through the flash, close enough to have taken it, and they walk home and begin to
     * die weeks later, infecting whoever nurses them. That is the ring where a world learns what
     * the bomb actually cost. */
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || u->sp >= SP_CIV_N) continue;
        int dx = (u->x >> 4) - cx, dy = (u->y >> 4) - cy, d2 = dx * dx + dy * dy;
        if (d2 < BLAST_BURN * BLAST_BURN) continue;                    /* dead or burning */
        if (d2 > (BLAST_BURN + 8) * (BLAST_BURN + 8)) continue;        /* out of the fallout */
        u->traits |= TR_PLAGUE | TR_CONTAGIOUS;
#if MOTE_HOST
        {   extern uint32_t mb_dosed; mb_dosed++; }
#endif
    }

    if (from_k > 0 && from_k < MAXK && mb_k[from_k].alive) mb_k[from_k].nuked++;
    mb_chron_disaster("the sky burns", cx, cy);
#if MOTE_HOST
    fprintf(stderr, "NUCLEAR STRIKE by kingdom %d at %d,%d\n", from_k, cx, cy);
#endif
}
