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

static uint8_t obj_fuel(uint8_t o)
{
    switch (o) {
    case O_DEAD:                return 255;   /* deadwood is tinder */
    case O_TREE: case O_TREE2:  return 220;
    case O_BUSH: case O_CACTUS: return 150;
    case O_TUFT: case O_FLOWER: return 110;
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

/* Fire intensity IS the fuel left to burn, so where it lands decides how long it
 * lives: a forest cell with a tree burns for 14 ticks, bare grass for 4. The
 * first model gave every cell a flat 10 and decayed it, which made a fire behave
 * identically in a rainforest and on a lawn — and, because a cast blob's edge
 * intensity fell off, made the FRONT the weakest part of the fire, so it always
 * guttered out after ten ticks instead of running. */
static int fuel_inten(int fuel)
{
    int i = fuel >> 5;
    return i < 2 ? 2 : (i > 15 ? 15 : i);
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

void mb_flux_step(void)
{
    uint8_t *flux = mb_w.flux, *next = s_next;
    uint8_t *bio = mb_w.biome, *ob = mb_w.obj, *elv = mb_w.elev;
    int wdx = WDX[s_wind_phase], wdy = WDY[s_wind_phase];

    wind_step();
    agent_step();          /* walking disasters seed flux before the pass reads it */
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
                    if (o && obj_fuel(o) && (r & 63) < 26) ob[i0] = O_NONE;
                    inten -= 1;
                    for (int k = 0; k < 4; k++) {
                        int nx = x + DX4[k], ny = y + DY4[k];
                        if (!mb_in(nx, ny)) continue;
                        int ni = AT(nx, ny);
                        int nf = biome_fuel(bio[ni]) + obj_fuel(ob[ni]);
                        if (!nf) continue;
                        /* the chance depends on what is THERE and the wind, not on
                         * how much is left here — so a fire front does not weaken
                         * as it advances into fresh fuel */
                        int downwind = (DX4[k] == wdx && wdx) || (DY4[k] == wdy && wdy);
                        int chance = (nf * (downwind ? 4 : 2)) >> 3;
                        if ((int)((r >> (k * 4 + 6)) & 255) < chance)
                            add_to(next, nx, ny, FX_FIRE, fuel_inten(nf));
                    }
                }
                if (inten <= 0) bio[i0] = burnt_biome(b);
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
    a->hp = (kind >= AG_KAIJU0 && kind < AG_MAW) ? 900 : 0;
    if (kind >= AG_KAIJU0 && kind < AG_MAW) mb_chron_disaster("a titan walks", x, y);
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
            if ((r & 3) != 3) {                      /* three ticks in four it moves */
                a->x = (int16_t)(a->x + (tx > a->x ? 1 : (tx < a->x ? -1 : 0)));
                a->y = (int16_t)(a->y + (ty > a->y ? 1 : (ty < a->y ? -1 : 0)));
            }
            /* the angel is the one that heals rather than harms: it smites the
             * marked and the cursed, and blesses everyone else */
            if (a->kind == AG_ANGEL) {
                mb_unit_area(a->x, a->y, 3, UAP_HEAL, 0);
                mb_unit_area(a->x, a->y, 3, UAP_UNTRAIT, TR_CURSED | TR_MADNESS);
                mb_fx_burst((float)a->x, (float)a->y, 3, PK_STAR, FXE_HOLY, 2.0f, 0.8f);
            } else {
                mb_unit_area(a->x, a->y, 2, UAP_HURT, 60);
                /* buildings in reach come down; a phoenix burns instead */
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int bx = a->x + dx, by = a->y + dy;
                        if (!mb_in(bx, by)) continue;
                        if (a->kind == AG_PHOENIX) { add_to(mb_w.flux, bx, by, FX_FIRE, 12); continue; }
                        if (mb_is_build(mb_w.obj[AT(bx, by)])) {
                            mb_w.obj[AT(bx, by)] = O_NONE;
                            mb_w.biome[AT(bx, by)] = B_RUBBLE;
                        }
                    }
                mb_fx_burst((float)a->x, (float)a->y, 2, PK_SMOKE, FXE_VOID, 1.5f, 0.9f);
            }
            mb_fx_shake(1.6f);
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

    /* TSUNAMI: rare, and it comes from the sea rather than from nowhere — a wall
     * of water pushed inland from a random coast, which then recedes through the
     * flood rule and leaves wet sand and drowned fields behind. */
    if ((r & 0x1FFFu) == 0) {
        int cx = (int)((r >> 13) % MW), cy = (int)((r >> 20) % MH);
        /* walk out to the nearest deep water so it starts offshore */
        for (int step = 0; step < 40 && mb_in(cx, cy); step++) {
            if (mb_w.biome[AT(cx, cy)] == B_OCEAN || mb_w.biome[AT(cx, cy)] == B_SEA) break;
            cx += (cx < MW / 2) ? -1 : 1;
        }
        if (mb_in(cx, cy) && mb_water(mb_w.biome[AT(cx, cy)])) {
            for (int d = -8; d <= 8; d++) mb_flux_add(cx, cy + d, FX_WATER, 12);
            mb_chron_disaster("a tsunami", cx, cy);
        }
    }

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
