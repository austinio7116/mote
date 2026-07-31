/*
 * Motebox — civilisation: villages, lords, kingdoms, war.
 *
 * THREE BRAINS (DESIGN.md 7), each on its own clock, because they decide things
 * at different rates and paying for all three every tick would be waste:
 *
 *   unit    (mb_unit.c)  1/8 of the population per tick — what shall I do now
 *   village  here        one village per tick           — what shall we build
 *   kingdom  here        one kingdom every 32 ticks     — war, peace, or neither
 *
 * The village brain does not command units. It publishes a NEED, and the unit
 * brain weighs that need against hunger and fear like any other option. A
 * villager who is starving ignores the lord, which is the behaviour you want and
 * it falls out of the utility score rather than being special-cased.
 */
#include "mb.h"
#include <stdio.h>
#include <string.h>

/* --- data --------------------------------------------------------------- */

Village mb_v[MAXV];
Kingdom mb_k[MAXK];

int mb_seaborne_colonies;        /* counted for the sweep, host only */
static int s_last_founded;       /* so a settler party can join what it just founded */
static uint8_t *s_field;         /* MAXV commute fields, 32x32 each */
#define FIELD_W 32
#define FIELD(v) (s_field + (size_t)((v) - 1) * FIELD_W * FIELD_W)
static uint16_t *s_bfsq;

void mb_civ_init(void)
{
    s_field = (uint8_t *)g_api->alloc((size_t)MAXV * FIELD_W * FIELD_W);
    s_bfsq  = (uint16_t *)g_api->alloc(FIELD_W * FIELD_W * sizeof(uint16_t));
    mb_civ_reset();
}

void mb_civ_reset(void)
{
    memset(mb_v, 0, sizeof mb_v);
    memset(mb_k, 0, sizeof mb_k);
    memset(mb_w.claim, 0, NC);
    s_last_founded = 0;
}

int mb_kingdom_of(int v) { return (v > 0 && v < MAXV && mb_v[v].alive) ? mb_v[v].kingdom : 0; }

int mb_at_war(int a, int b)
{
    if (!a || !b || a == b) return 0;
    return (mb_k[a].war_with >> b) & 1;
}

int mb_village_count(void) { int n = 0; for (int i = 1; i < MAXV; i++) if (mb_v[i].alive) n++; return n; }
int mb_kingdom_count(void) { int n = 0; for (int i = 1; i < MAXK; i++) if (mb_k[i].alive) n++; return n; }

/* --- the commute field --------------------------------------------------
 * Each village owns a 32x32 BFS distance field to its hall, rebuilt when its
 * buildings change — one village per tick, amortised. 95% of all movement is a
 * villager inside its own territory following this gradient, which is O(1) per
 * unit per step. Long journeys (settlers, armies) do not use it: they steer at
 * the block level, because a 32x32 window cannot reach across a continent. */
static void field_build(int v)
{
    Village *V = &mb_v[v];
    uint8_t *f = FIELD(v);
    memset(f, 0xFF, FIELD_W * FIELD_W);
    int ox = V->x - FIELD_W / 2, oy = V->y - FIELD_W / 2;
    int head = 0, tail = 0;
    int sx = FIELD_W / 2, sy = FIELD_W / 2;
    f[sy * FIELD_W + sx] = 0;
    s_bfsq[tail++] = (uint16_t)(sy * FIELD_W + sx);
    while (head < tail) {
        int c = s_bfsq[head++];
        int cx = c % FIELD_W, cy = c / FIELD_W;
        uint8_t nd = (uint8_t)(f[c] < 250 ? f[c] + 1 : 250);
        for (int k = 0; k < 4; k++) {
            static const int8_t DX[4] = { 1, -1, 0, 0 };
            static const int8_t DY[4] = { 0, 0, 1, -1 };
            int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || ny < 0 || nx >= FIELD_W || ny >= FIELD_W) continue;
            int ni = ny * FIELD_W + nx;
            if (f[ni] != 0xFF) continue;
            int wx = ox + nx, wy = oy + ny;
            if (!mb_in(wx, wy) || !mb_land(mb_w.biome[AT(wx, wy)])) continue;
            f[ni] = nd;
            s_bfsq[tail++] = (uint16_t)ni;
        }
    }
    V->dirty = 0;
}

/* One step down a village's commute gradient. Returns 0 if the cell is outside
 * the window or unreachable, and the caller falls back to steering. */
int mb_village_step_home(int v, int x, int y, int *ox, int *oy)
{
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) return 0;
    Village *V = &mb_v[v];
    int fx = x - (V->x - FIELD_W / 2), fy = y - (V->y - FIELD_W / 2);
    if (fx < 1 || fy < 1 || fx >= FIELD_W - 1 || fy >= FIELD_W - 1) return 0;
    const uint8_t *f = FIELD(v);
    int here = f[fy * FIELD_W + fx];
    if (here == 0xFF) return 0;
    int best = here, bx = x, by = y;
    for (int k = 0; k < 4; k++) {
        static const int8_t DX[4] = { 1, -1, 0, 0 };
        static const int8_t DY[4] = { 0, 0, 1, -1 };
        int nd = f[(fy + DY[k]) * FIELD_W + (fx + DX[k])];
        if (nd < best) { best = nd; bx = x + DX[k]; by = y + DY[k]; }
    }
    *ox = bx; *oy = by;
    return best < here;
}

/* --- founding -----------------------------------------------------------
 * WorldBox's rule, scaled: a settlement needs a filled square of buildable land
 * and an island big enough to be worth settling. Both matter — without the
 * island test, villages appear on three-tile rocks and starve; without the zone
 * test, a village founds itself on a cliff edge and has nowhere to build. */
#define ZONE 6

static int buildable(int x, int y)
{
    if (!mb_in(x, y)) return 0;
    uint8_t b = mb_w.biome[AT(x, y)];
    if (!mb_land(b)) return 0;
    if (b == B_MOUNTAIN || b == B_PEAK) return 0;
    return 1;
}

static int zone_ok(int cx, int cy)
{
    int n = 0;
    for (int y = cy - ZONE / 2; y < cy + ZONE / 2; y++)
        for (int x = cx - ZONE / 2; x < cx + ZONE / 2; x++)
            if (buildable(x, y)) n++;
    return n >= ZONE * ZONE - 4;
}

/* Flood-count the connected land around a cell, giving up at `cap`. Cheap
 * because it stops as soon as the answer is "big enough". */
static uint8_t  s_seen[NC];      /* flood-fill marks; .bss, not the arena */
static uint16_t s_flood[2048];

static int landmass_at_least(int cx, int cy, int cap)
{
    memset(s_seen, 0, NC);
    int head = 0, tail = 0, n = 0;
    s_flood[tail++] = (uint16_t)AT(cx, cy);
    s_seen[AT(cx, cy)] = 1;
    while (head < tail && n < cap) {
        int c = s_flood[head++];
        n++;
        int x = c % MW, y = c / MW;
        for (int k = 0; k < 4; k++) {
            static const int8_t DX[4] = { 1, -1, 0, 0 };
            static const int8_t DY[4] = { 0, 0, 1, -1 };
            int nx = x + DX[k], ny = y + DY[k];
            if (!mb_in(nx, ny) || s_seen[AT(nx, ny)]) continue;
            if (!mb_land(mb_w.biome[AT(nx, ny)])) continue;
            s_seen[AT(nx, ny)] = 1;
            if (tail < (int)(sizeof s_flood / sizeof s_flood[0]))
                s_flood[tail++] = (uint16_t)AT(nx, ny);
        }
    }
    return n >= cap;
}

int mb_village_found(int sp, int cx, int cy, int kingdom)
{
    if (sp < 0 || sp >= SP_CIV_N) return 0;
    if (!zone_ok(cx, cy) || !landmass_at_least(cx, cy, 120)) return 0;
    if (mb_w.claim[AT(cx, cy)]) return 0;

    int v = 0;
    for (int i = 1; i < MAXV; i++) if (!mb_v[i].alive) { v = i; break; }
    if (!v) return 0;

    Village *V = &mb_v[v];
    memset(V, 0, sizeof *V);
    V->alive = 1; V->sp = (uint8_t)sp;
    V->x = (uint8_t)cx; V->y = (uint8_t)cy;
    V->hall = 1; V->loyalty = 70; V->dirty = 1;
    V->founded = mb_w.tick;
    V->name = mb_name_place(v * 7919u + mb_w.seed);

    /* THE LORD. Three stats and a trait roll, WorldBox's shape, and the cheapest
     * way to make two villages of the same race feel unalike: stewardship decides
     * how well it builds, diplomacy how well it holds its people, warfare how hard
     * it musters. They were all zero in the first version, which is why loyalty
     * never fell far enough for a single rebellion in four hundred years — the one
     * story generator in the design was silently switched off. */
    {
        uint32_t lr = mb_rand((uint32_t)v * 40503u + mb_w.seed);
        V->lord_diplo = (uint8_t)(lr % 100);
        V->lord_stew  = (uint8_t)((lr >> 8) % 100);
        V->lord_war   = (uint8_t)((lr >> 16) % 100);
        if (((lr >> 24) & 7) < 2) V->lord_traits |= TR_AMBITIOUS;
        if (((lr >> 27) & 7) < 2) V->lord_traits |= TR_LOYAL;
        V->lord_name = mb_name_person(lr ^ 0x51ed2701u);
        V->lord_age  = (uint8_t)(24 + (lr >> 5) % 20u);
    }
    V->creed = 0;                /* creed_step() will hear a neighbour or invent one */
    V->spec  = PROF_FARMER;      /* until specialise() has looked at the land */
    V->tier  = TIER_CAMP;

    /* a kingdom of its own unless it is a colony of one that exists */
    if (kingdom > 0 && kingdom < MAXK && mb_k[kingdom].alive) V->kingdom = (uint8_t)kingdom;
    else {
        int k = 0;
        for (int i = 1; i < MAXK; i++) if (!mb_k[i].alive) { k = i; break; }
        if (!k) { V->alive = 0; return 0; }
        memset(&mb_k[k], 0, sizeof mb_k[k]);
        mb_k[k].alive = 1; mb_k[k].sp = (uint8_t)sp;
        mb_k[k].colour = (uint8_t)(k % 5);          /* the sheet's five banner rows */
        mb_k[k].capital = (uint8_t)v;
        mb_k[k].name = mb_name_kingdom(k * 104729u + mb_w.seed);
        V->kingdom = (uint8_t)k;
    }

    /* the founding marker, and the claim */
    mb_w.obj[AT(cx, cy)] = O_FIRE_PIT;
    for (int y = cy - ZONE / 2; y <= cy + ZONE / 2; y++)
        for (int x = cx - ZONE / 2; x <= cx + ZONE / 2; x++)
            if (mb_in(x, y) && !mb_w.claim[AT(x, y)] && mb_land(mb_w.biome[AT(x, y)]))
                mb_w.claim[AT(x, y)] = (uint8_t)v;

    field_build(v);
    s_last_founded = v;
    mb_chron_found(v);
    return v;
}

/* THE STONE DEADLOCK, which MOTEBOX_LOOPS found by printing something nobody had looked
 * at: every village in a 400-year world still had a TIER-ONE HALL. A mine cost ten stone
 * and stone comes only from mines, and a woodcutter's camp cost four stone as well — so a
 * village that started with none could never build the thing that produces it. Everything
 * above a timber hall was therefore unreachable content: no tier-two hall, so no temple,
 * no wall, no castle, no city. The pithead and the camp are TIMBER-FRAMED now, which is
 * what they would be, and the ladder connects to the ground.
 *
 * --- the building ladder ------------------------------------------------
 * WorldBox's costs, near enough verbatim, because they are well tuned and they
 * make a stockpile legible: you can look at a village's store and know what it
 * is about to do. */
typedef struct { uint8_t obj, wood, stone, iron, gold, cap, tech, grand; const char *name; } BuildDef;
static const BuildDef BUILD[] = {
    /* The last column is the TECH a kingdom must know. This is where research stops being
     * a number in the HUD and becomes architecture: an ignorant kingdom's towns are timber
     * and thatch for ever, and you can see at a glance which crown is going somewhere. */
    { O_HOUSE1,   4,  0,  0,  0, 40, TECH_NONE,   0, "house"    },
    { O_HOUSE2,   4,  4,  0,  0, 40, TECH_TOOLS,  0, "house"    },
    { O_HOUSE3,  10, 10, 10, 10, 40, TECH_MASONRY,0, "house"    },
    { O_FARM,     6,  0,  0,  0,  4, TECH_NONE,   0, "farm"     },
    { O_WOODCUT,  8,  0,  0,  0,  2, TECH_TOOLS,  0, "camp"     },
    { O_MINE,    12,  0,  0,  0,  1, TECH_TOOLS,  0, "mine"     },
    { O_BARRACKS, 8, 12,  4,  0,  1, TECH_BRONZE, 0, "barracks" },
    { O_TEMPLE,  10, 14,  0,  4,  1, TECH_WRITING, 0, "temple"  },
    { O_TOWER,    6, 12,  0,  0,  3, TECH_MASONRY,0, "tower"    },
    { O_HALL2,   10, 10,  0,  0,  1, TECH_TOOLS,  0, "hall"     },
    { O_HALL3,   10, 10, 10,  0,  1, TECH_MASONRY,0, "castle"   },
    { O_WALL,     2, 10,  0,  0, 60, TECH_MASONRY,0, "wall"     },   /* stone, and a lot of it */
    { O_DOCK,     8,  4,  0,  0,  2, TECH_SEAFARING, 0, "dock"  },
    /* THE CIVIC LADDER, one rung per era. Priced in timber and stone because those are the
     * only currencies a town can actually bank (see the note on the silo), and capped low so
     * a city gets one of each rather than a row of them. */
    { O_GRANARY,  8,  0,  0,  0,  2, TECH_POTTERY,    0, "granary"  },
    { O_MARKET,  10,  6,  0,  0,  2, TECH_CURRENCY,   0, "market"   },
    { O_LIBRARY,  6, 14,  0,  0,  1, TECH_WRITING,    0, "library"  },
    { O_FOUNDRY, 12, 20,  0,  0,  1, TECH_METALLURGY, 0, "foundry"  },
    { O_COLLEGE, 14, 26,  0,  0,  1, TECH_UNIVERSITY, 1, "college"  },
    { O_HOSPITAL,12, 18,  0,  0,  1, TECH_SANITATION, 0, "hospital" },
    { O_FACTORY, 18, 24,  0,  0,  2, TECH_STEAM,      1, "factory"  },
    { O_STATION, 14, 18,  0,  0,  1, TECH_RAILWAY,    1, "station"  },
    { O_POWER,   10, 30,  0,  0,  1, TECH_ELECTRIC,   1, "power"    },
    /* THE SILO. The tree has to end somewhere you can see, and a kingdom that has spent
     * three thousand research on the bomb should have something standing to show it. One per
     * city, stone and iron and a great deal of gold. */
    /* COSTED IN WHAT ACTUALLY EXISTS. Three attempts at this failed for the same reason and
     * it took an inspect dump to see it: a 600-year city holds grain 600, timber 2, stone 0,
     * IRON 0, GOLD 0. There is no iron or gold income in this economy at all — miners gather
     * but nothing delivers it — so any cost in those currencies can never be paid, and two
     * kingdoms with the bomb had no silo between them across five worlds. Timber and stone
     * are the only real currencies, so a silo is priced in those: a huge concrete project,
     * paid for over a generation by the blueprint machinery below.
     *
     * (The missing iron and gold income is a separate defect and is worth its own pass.) */
    { O_SILO,    20, 40,  0,  0,  1, TECH_NUKE,      1, "silo"  },
};
#define NBUILD ((int)(sizeof BUILD / sizeof BUILD[0]))

int mb_civ_count(int v, uint8_t obj)
{
    Village *V = &mb_v[v];
    int n = 0;
    for (int y = V->y - 8; y <= V->y + 8; y++)
        for (int x = V->x - 8; x <= V->x + 8; x++)
            if (mb_in(x, y) && mb_w.claim[AT(x, y)] == v && mb_w.obj[AT(x, y)] == obj) n++;
    return n;
}

/* ======================================================================
 * THE TOWN-DEVELOPMENT LOOPS
 *
 * A god sim with no systems under it is a paint program: you can set a forest alight and
 * watch it burn, and nothing that happens afterwards is different from what happened
 * before. What makes a world worth watching is the loops feeding each other — the land
 * decides what a town is good at, what it is good at decides what it builds, what it
 * builds decides what its kingdom learns, what the kingdom learns decides what its towns
 * may attempt, and a surplus in one town becomes a caravan to another.
 *
 * Each of the following is small on its own. They are worth having because they COMPOSE.
 * ====================================================================== */

const char *const MB_PROF_NAME[PROF_N] = {
    "-", "farmer", "forester", "miner", "soldier", "priest", "trader", "builder", "lord",
};
const char *const MB_CREED_NAME[CREED_N] = { "-", "SUN", "MOON", "STONE", "TIDE" };
const char *const MB_TIER_NAME[TIER_N] = { "camp", "hamlet", "village", "TOWN", "CITY" };
const char *const MB_ERA_NAME[ERA_N] = {
    "STONE", "BRONZE", "IRON", "CLASSICAL", "MEDIEVAL",
    "RENAISSANCE", "INDUSTRIAL", "MODERN", "ATOMIC",
};

/* THE TREE. Costs rise by era; prerequisites are what make it a graph rather than a queue,
 * and what makes two kingdoms in the same world develop differently — each takes what it
 * needs, so a besieged crown ends up with gunpowder and metallurgy while a rich coastal one
 * has navigation, banking and economics. */
#define P2(a, b) { a, b }
#define P1(a)    { a, TECH_NONE }
#define P0       { TECH_NONE, TECH_NONE }
const TechDef MB_TECH[TECH_N] = {
    /* name            era               cost  prerequisites */
    { "-",             ERA_STONE,           0, P0 },
    { "tools",         ERA_STONE,          270, P0 },
    { "agriculture",   ERA_STONE,         330, P0 },
    { "pottery",       ERA_STONE,         360, P0 },

    { "masonry",       ERA_BRONZE,        570, P1(TECH_TOOLS) },
    { "bronze",        ERA_BRONZE,        600, P1(TECH_TOOLS) },
    { "the wheel",     ERA_BRONZE,        540, P1(TECH_TOOLS) },
    { "writing",       ERA_BRONZE,        660, P1(TECH_POTTERY) },

    { "ironwork",      ERA_IRON,          960, P1(TECH_BRONZE) },
    { "seafaring",     ERA_IRON,          900, P2(TECH_WHEEL, TECH_POTTERY) },
    { "architecture",  ERA_IRON,          1020, P1(TECH_MASONRY) },
    { "currency",      ERA_IRON,          930, P1(TECH_WRITING) },

    { "engineering",   ERA_CLASSICAL,     1380, P2(TECH_ARCHITECTURE, TECH_IRON) },
    { "mathematics",   ERA_CLASSICAL,     1320, P2(TECH_WRITING, TECH_CURRENCY) },
    { "cavalry",       ERA_CLASSICAL,     1260, P2(TECH_IRON, TECH_WHEEL) },
    { "law",           ERA_CLASSICAL,     1350, P2(TECH_WRITING, TECH_AGRICULTURE) },

    { "gunpowder",     ERA_MEDIEVAL,      1860, P2(TECH_IRON, TECH_MATHS) },
    { "navigation",    ERA_MEDIEVAL,      1800, P2(TECH_SEAFARING, TECH_MATHS) },
    { "banking",       ERA_MEDIEVAL,      1770, P2(TECH_CURRENCY, TECH_LAW) },
    { "the university",ERA_MEDIEVAL,      640, P2(TECH_MATHS, TECH_LAW) },

    { "printing",      ERA_RENAISSANCE,   2400, P2(TECH_UNIVERSITY, TECH_WRITING) },
    { "metallurgy",    ERA_RENAISSANCE,   2580, P2(TECH_GUNPOWDER, TECH_ENGINEER) },
    { "sanitation",    ERA_RENAISSANCE,   2460, P2(TECH_UNIVERSITY, TECH_ENGINEER) },
    { "economics",     ERA_RENAISSANCE,   2340, P2(TECH_BANKING, TECH_UNIVERSITY) },

    { "steam",         ERA_INDUSTRIAL,   3150, P2(TECH_METALLURGY, TECH_ECONOMICS) },
    { "the railway",   ERA_INDUSTRIAL,   3300, P2(TECH_STEAM, TECH_ENGINEER) },
    { "chemistry",     ERA_INDUSTRIAL,   3240, P2(TECH_SANITATION, TECH_METALLURGY) },
    { "electricity",   ERA_INDUSTRIAL,   3360, P2(TECH_STEAM, TECH_PRINTING) },

    { "combustion",    ERA_MODERN,       4200, P2(TECH_STEAM, TECH_CHEMISTRY) },
    { "flight",        ERA_MODERN,       4500, P2(TECH_COMBUSTION, TECH_ELECTRIC) },
    { "radio",         ERA_MODERN,       4140, P2(TECH_ELECTRIC, TECH_PRINTING) },
    { "medicine",      ERA_MODERN,       4320, P2(TECH_SANITATION, TECH_CHEMISTRY) },

    { "physics",       ERA_ATOMIC,       5700, P2(TECH_ELECTRIC, TECH_MATHS) },
    { "fission",       ERA_ATOMIC,       6900, P2(TECH_PHYSICS, TECH_CHEMISTRY) },
    { "rocketry",      ERA_ATOMIC,       6600, P2(TECH_FLIGHT, TECH_PHYSICS) },
    { "the bomb",      ERA_ATOMIC,       9000, P2(TECH_FISSION, TECH_ROCKETRY) },
};
#undef P0
#undef P1
#undef P2
/* A short table is silent, and this game has been bitten by that twice. */
typedef char mb_techdef_complete[(sizeof MB_TECH / sizeof MB_TECH[0]) == TECH_N ? 1 : -1];

const char *const MB_TECH_NAME[TECH_N] = {
    "-", "tools", "agriculture", "pottery",
    "masonry", "bronze", "wheel", "writing",
    "ironwork", "seafaring", "architecture", "currency",
    "engineering", "maths", "cavalry", "law",
    "gunpowder", "navigation", "banking", "university",
    "printing", "metallurgy", "sanitation", "economics",
    "steam", "railway", "chemistry", "electricity",
    "combustion", "flight", "radio", "medicine",
    "physics", "fission", "rocketry", "THE BOMB",
};
typedef char mb_technames_complete[(sizeof MB_TECH_NAME / sizeof MB_TECH_NAME[0]) == TECH_N ? 1 : -1];

/* WHAT A KINGDOM'S SOLDIERS FIRE, and how far. This is the tech tree arriving in the one place
 * a player is already looking. Everyone can throw a rock; a bow needs bronze; a musket needs
 * gunpowder; a field gun needs metallurgy; a rocket needs rocketry. Range rises with each,
 * which is why an army that has crossed a weapons era beats one that has not for a reason you
 * can watch rather than one you have to infer from hit points. */
int mb_war_shot_kind(int k)
{
    if (mb_tech_known(k, TECH_ROCKETRY))  return MB_SHOT_MISSILE;
    if (mb_tech_known(k, TECH_METALLURGY))return MB_SHOT_SHELL;
    if (mb_tech_known(k, TECH_GUNPOWDER)) return MB_SHOT_BULLET;
    if (mb_tech_known(k, TECH_BRONZE))    return MB_SHOT_ARROW;
    return MB_SHOT_ROCK;
}

int mb_war_shot_range(int kind)
{
    /* A THROWN ROCK AT THREE CELLS is not a missile, it is a shove: melee begins at one and a
     * half, so the stone age had a one-cell firing window and effectively no ranged combat.
     * Four is enough to stand off in, and the ladder still more than triples by the end. */
    static const uint8_t R[MB_SHOT_N] = { 4, 6, 8, 10, 13 };
    return R[(kind >= 0 && kind < MB_SHOT_N) ? kind : 0];
}

int mb_tech_known(int k, int t)
{
    if (k <= 0 || k >= MAXK || !mb_k[k].alive) return t <= TECH_NONE;
    return (mb_k[k].known & TECHB(t)) != 0;
}

int mb_tech_avail(int k, int t)
{
    if (t <= TECH_NONE || t >= TECH_N) return 0;
    if (mb_tech_known(k, t)) return 0;
    for (int i = 0; i < 2; i++) {
        int need = MB_TECH[t].need[i];
        if (need != TECH_NONE && !mb_tech_known(k, need)) return 0;
    }
    return 1;
}

/* --- TIER: what kind of place this is ---------------------------------
 * From population and civic fabric together, because either alone lies: forty people in
 * huts is a big hamlet, and a stone hall with nine residents is a lord's folly. */
int mb_civ_tier(int v)
{
    Village *V = &mb_v[v];
    if (!V->alive) return TIER_CAMP;
    int pop = V->pop, hall = V->hall;
    if (pop >= 34 && hall >= 3) return TIER_CITY;
    if (pop >= 20 && hall >= 2) return TIER_TOWN;
    if (pop >= 10)              return TIER_VILLAGE;
    if (pop >= 4)               return TIER_HAMLET;
    return TIER_CAMP;
}

int mb_civ_tech_ok(int v, int need)
{
    if (need <= TECH_NONE) return 1;
    return mb_tech_known(mb_v[v].kingdom, need);
}

/* --- RESEARCH: a kingdom learns from its towns ------------------------
 * Kingdom.tech existed as a field, was incremented by exactly one power, and was read by
 * nothing. So a kingdom could not develop — every town in the world had the same ceiling
 * on the first tick as on the five hundredth.
 *
 * Now temples, scholars-in-numbers and gold all push a village's research, the kingdom
 * banks it, and each tech costs more than the last. Which means a rich, populous, pious
 * kingdom really does out-develop a poor one, and you can see it in their architecture.
 */
/* --- THE CIVIC LADDER --------------------------------------------------
 *
 * A town used to look identical in the Stone era and the Atomic one — thatch, a hall, a
 * temple, a wall — whatever its kingdom had spent twenty thousand research points on. These
 * nine buildings are what the tree BUILDS, so a city dates itself: timber granary, stone
 * library with pillars, brick foundry with a stack, then concrete cooling towers.
 *
 * It gets its OWN TURN, and by now that is a familiar shape. The lord takes the first
 * affordable want and stops, so anything at the back of the list is built only when nothing
 * ahead of it is wanted — and beds, bread and timber always are. The wall, the dock and the
 * silo each died that way before being given a turn; there was no reason to expect nine more
 * to fare better. This one picks the MOST ADVANCED rung the town lacks, so a city climbs the
 * ladder in era order rather than filling in the cheap end for ever.
 */
static const uint8_t CIVIC_WANT[9] = { 13, 14, 15, 16, 17, 18, 19, 20, 21 };

static int civic_pick(int v)
{
    for (int i = 8; i >= 0; i--) {                 /* newest first: a city modernises */
        const BuildDef *B = &BUILD[CIVIC_WANT[i]];
        if (!mb_civ_tech_ok(v, B->tech)) continue;
        if (mb_civ_count(v, B->obj) >= B->cap) continue;
        return CIVIC_WANT[i];
    }
    return -1;
}

/* What the civic buildings PRODUCE. Two of these exist because the economy had a hole in it:
 * an inspect dump of a 600-year city read iron 0, gold 0 — miners gathered and nothing ever
 * delivered — so no cost in those currencies could ever be paid. The market and the foundry
 * are now the only sources of either, which also means a kingdom that never reaches currency
 * or metallurgy simply has no treasury, and that is a fair consequence. */
/* Lifetime yield, for the audit. The STANDING balance says nothing: iron is spent the tick
 * it lands and gold is spent by nothing at all, so the same number means "healthy" for one
 * and "runaway" for the other. */
int mb_mined_iron, mb_mined_gold;

static void civic_yield(int v)
{
    Village *V = &mb_v[v];
    int k = V->kingdom;
    int mk = mb_civ_count(v, O_MARKET), fo = mb_civ_count(v, O_FOUNDRY);
    int gr = mb_civ_count(v, O_GRANARY), fa = mb_civ_count(v, O_FACTORY);
    if (mk) V->gold  = (uint16_t)(V->gold  + mk * (mb_tech_known(k, TECH_BANKING) ? 2 : 1));
    if (fo) V->iron  = (uint16_t)(V->iron  + fo * (mb_tech_known(k, TECH_STEAM)   ? 2 : 1));
    if (gr) V->food  = (uint16_t)(V->food  + gr * 2);        /* a store keeps what it holds */
    if (fa) { V->wood  = (uint16_t)(V->wood  + fa * 2);
              V->stone = (uint16_t)(V->stone + fa * 2); }
    /* A MINE THAT MINES. The foundry and the market were the only sources of iron and gold in
     * the world, both flat and both indifferent to the ground they stood on — so a kingdom
     * sitting on a mountain of ore was no richer than one on a sandbank, and the ore, silver,
     * gold and gem objects worldgen scatters were decoration. A mine now yields what is
     * actually under it, and a metallurgist kingdom gets more out of the same seam.
     *
     * It READS the seams rather than consuming them: a mine is a hole that keeps producing,
     * and an income that deletes its own source is not an income. */
    /* A MINE THAT MINES. Measured from EACH MINE, not from the village centre: the first
     * version scanned six cells around the hall, so whether a kingdom had a metal industry
     * depended on whether worldgen happened to drop a seam near its market square. Lifetime
     * yield froze at 279 iron and zero gold across seven hundred years — gold seams sit on
     * mountain cells and a village never builds within six of one.
     *
     * So a mine is a hole in the ground where somebody chose to dig, and it yields IRON as a
     * baseline because that is what a mine is for; a seam of gold, silver or gems in reach of
     * the shaft adds gold on top. It reads the seams rather than consuming them — an income
     * that deletes its own source is not an income. */
    int mi = 0, seam_iron = 0, seam_gold = 0;
    for (int dy = -12; dy <= 12; dy++)
        for (int dx = -12; dx <= 12; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y) || mb_w.obj[AT(x, y)] != O_MINE) continue;
            if (mb_w.claim[AT(x, y)] != v) continue;
            mi++;
            /* FIVE, not three. At three, no mine in a seven-hundred-year world ever had a
             * gold, silver or gem seam in reach — they sit on mountain cells and a mine is
             * sited for stone near the town. Five reaches the scarp. */
            for (int sy = -5; sy <= 5; sy++)
                for (int sx = -5; sx <= 5; sx++) {
                    int px = x + sx, py = y + sy;
                    if (!mb_in(px, py)) continue;
                    uint8_t o = mb_w.obj[AT(px, py)];
                    if (o == O_ORE) seam_iron++;
                    else if (o == O_GOLD || o == O_SILVER || o == O_GEM) seam_gold++;
                }
        }
    if (mi) {
        int rich = mb_tech_known(k, TECH_METALLURGY) ? 2 : 1;
        int yi = rich * (mi + seam_iron / 4);          /* every mine makes iron */
        int yg = seam_gold ? rich * (1 + seam_gold / 4) : 0;
        V->iron = (uint16_t)(V->iron + yi);
        V->gold = (uint16_t)(V->gold + yg);
        mb_mined_iron += yi; mb_mined_gold += yg;
    }
    /* NO UPKEEP. A wear term was tried here — a town spending timber and stone in proportion
     * to how much of it there is — on the theory that a stockpile filled once and never falling
     * is why wants dry up. It does dry them up, but the cure was worse: at built/4 a mature town
     * spent faster than it could gather and populations fell from 222 to 27 by year 600; at
     * built/6 they still fell to 123. Standing still costing nothing is the lesser wrong.
     */
    if (V->gold  > 900) V->gold  = 900;
    if (V->iron  > 900) V->iron  = 900;
}

/* WHICH TECH A KINGDOM CHOOSES is what makes two of them diverge. Cheapest-available would
 * make every crown follow the same path again, so the pick is weighted by circumstance: a
 * kingdom at war reaches for weapons, a coastal one for the sea, a rich one for money, and a
 * plagued one for medicine. The result is that the same world grows a naval power and a
 * gunpowder power out of the same tree. */
static int research_pick(int k)
{
    const Kingdom *K = &mb_k[k];
    /* bestscore starts at the floor, not at -1. The score below is a BASE MINUS COST, so any
     * tech dearer than the base scores negative — and with -1 as the initial best, every one
     * of those was rejected. Trebling the costs to slow the tree therefore made it
     * UNFINISHABLE: a kingdom reached 29 of 35 and then sat there with no goal from year 150
     * to year 400, which the loops report showed as "goal -" and nothing moving. */
    int best = TECH_NONE, bestscore = -(1 << 30);
    int atwar = K->war_with != 0, coastal = 0, rich = 0;
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive || mb_v[v].kingdom != k) continue;
        if (mb_v[v].coast >= 8) coastal = 1;
        if (mb_v[v].gold > 40) rich = 1;
    }
    for (int t = TECH_TOOLS; t < TECH_N; t++) {
        if (!mb_tech_avail(k, t)) continue;
        /* cheap is good — a kingdom takes the next rung it can reach */
        int score = 12000 - (int)MB_TECH[t].cost;   /* cheap first, but never negative */
        switch (t) {
        case TECH_BRONZE: case TECH_IRON: case TECH_CAVALRY:
        case TECH_GUNPOWDER: case TECH_METALLURGY: case TECH_COMBUSTION:
            if (atwar) score += 700; break;
        case TECH_SEAFARING: case TECH_NAVIGATION:
            if (coastal) score += 600; break;
        case TECH_CURRENCY: case TECH_BANKING: case TECH_ECONOMICS:
            if (rich) score += 400; break;
        case TECH_SANITATION: case TECH_MEDICINE:
            if (K->exhaustion > 40) score += 500; break;
        /* AND THE BOMB IS ALWAYS WANTED once it is in reach, which is the whole point of
         * putting it at the end of the tree rather than behind a die roll. */
        case TECH_FISSION: case TECH_ROCKETRY: case TECH_NUKE:
            score += 900; break;
        default: break;
        }
        /* a little per-kingdom bias, so two crowns in identical circumstances still differ */
        score += (int)(mb_rand((uint32_t)(k * 7717u + (uint32_t)t * 131u)) % 200u);
        if (score > bestscore) { bestscore = score; best = t; }
    }
    return best;
}

static void research_step(int v)
{
    Village *V = &mb_v[v];
    int k = V->kingdom;
    if (k <= 0 || k >= MAXK || !mb_k[k].alive) return;
    Kingdom *K = &mb_k[k];

    if (!K->goal || mb_tech_known(k, K->goal)) {
        K->goal = (uint8_t)research_pick(k);
        K->bank = 0;
        if (!K->goal) return;                       /* the tree is finished */
    }

    int gain = V->pop / 6;                          /* people thinking */
    gain += mb_civ_count(v, O_TEMPLE) * 3;          /* somewhere to write it down */
    /* AND THE BUILDINGS THAT EXIST TO DO THIS. A library is worth a temple and a half, a
     * college three of them, a power station four — which is why a kingdom that builds the
     * civic ladder out-researches one that only builds beds by a factor of several, and why
     * the deep eras are reachable at all. */
    /* MODEST, and measured. At 4/9/14 these created a runaway: research buys a power
     * station, the station pays fourteen points per village visit, that buys the next tech
     * sooner, and a kingdom completed all thirty-five techs BY YEAR 150 — the entire tree
     * gone before the first war. The road grades made it visible (tarmac and lane markings on
     * a year-150 town) but the cause was here. */
    gain += mb_civ_count(v, O_LIBRARY) * 2;
    gain += mb_civ_count(v, O_COLLEGE) * 4;
    gain += mb_civ_count(v, O_POWER)   * 6;
    if (V->gold > 20) gain += 1;                    /* and the means to pay for it */
    if (V->creed == K->creed && V->creed) gain += 1;
    /* THE TREE PAYS FOR ITSELF. Without these a thirty-six tech ladder costing twenty
     * thousand points is unreachable in four hundred years — the deep eras would be
     * decoration. Each is a tech whose whole purpose is to make research faster. */
    if (mb_tech_known(k, TECH_WRITING))    gain += 1;
    if (mb_tech_known(k, TECH_MATHS))      gain += 2;
    if (mb_tech_known(k, TECH_UNIVERSITY)) gain += 4;
    if (mb_tech_known(k, TECH_PRINTING))   gain += 4;
    if (mb_tech_known(k, TECH_ELECTRIC))   gain += 6;
    if (mb_tech_known(k, TECH_RADIO))      gain += 6;
    if (gain <= 0) return;

    /* SUB-LINEAR IN EMPIRE SIZE. Every town contributes independently, so a kingdom of eight
     * towns was researching eight times as fast as one of one — and the dominant kingdom in a
     * measured world reached the ATOMIC era by YEAR 150 while its neighbours were still on
     * pottery. A bigger realm is harder to coordinate, not proportionally cleverer: with this,
     * eight towns are worth about twice one rather than eight times, and the leader arrives at
     * the end of the tree in the sixth century instead of the second. */
    {
        int towns = 0;
        for (int i = 1; i < MAXV; i++) if (mb_v[i].alive && mb_v[i].kingdom == k) towns++;
        gain = gain * 3 / (3 + (towns > 1 ? towns : 1));
        if (gain <= 0) gain = 1;
    }

    K->bank = (uint16_t)(K->bank + gain);
    if (K->bank < MB_TECH[K->goal].cost) return;

    K->known |= TECHB(K->goal);
    K->tech++;
    K->bank = 0;
    /* the era is the highest one it has anything from */
    int era = ERA_STONE;
    for (int t = TECH_TOOLS; t < TECH_N; t++)
        if (mb_tech_known(k, t) && MB_TECH[t].era > era) era = MB_TECH[t].era;
    K->era = (uint8_t)era;
    mb_chron_disaster(MB_TECH_NAME[K->goal], V->x, V->y);
    K->goal = (uint8_t)research_pick(k);
}

/* --- SPECIALISATION: the land decides what a town is for --------------
 * Every village ran the same want list, so every village came out the same shape whether
 * it sat on an orefield or a floodplain. This counts what is actually within reach and
 * names the town's trade — and the want list below then leans that way, so a mountain
 * village really does become a mining town while its neighbour on the grass grows farms.
 */
void mb_civ_specialise(int v)
{
    Village *V = &mb_v[v];
    int ore = 0, wood = 0, water = 0, soil = 0;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y)) continue;
            int i = AT(x, y);
            uint8_t b = mb_w.biome[i], o = mb_w.obj[i];
            if (o == O_ORE || o == O_SILVER || o == O_GOLD || o == O_GEM) ore += 2;
            if (b == B_MOUNTAIN || b == B_HILL || b == B_PEAK) ore++;
            if (o == O_TREE || o == O_TREE2) wood += 2;
            if (b == B_FOREST) wood++;
            if (mb_water(b)) water++;
            if (b == B_GRASS || b == B_MEADOW || b == B_SAVANNA || b == B_FARM) soil++;
        }
    /* A COAST IS A COAST. The first version required TECH_SEAFARING for a town to count
     * as a trading one, and the result was zero trade towns in a four-hundred-year world
     * even after a kingdom learned to sail — identity should come from the LAND, and tech
     * should gate what you may BUILD on it. So a well-watered town is a fishing village
     * from the day it is founded, and the dock arrives when the kingdom works out how. */
    V->coast = (uint8_t)(water > 255 ? 255 : water);
    int trade = (water >= 12) ? water : 0;
    if (water >= 12 && mb_civ_tech_ok(v, TECH_SEAFARING)) trade += water / 2;
    int best = soil; V->spec = PROF_FARMER;
    if (wood  > best) { best = wood;  V->spec = PROF_FORESTER; }
    if (ore   > best) { best = ore;   V->spec = PROF_MINER;    }
    if (trade > best) { best = trade; V->spec = PROF_TRADER;   }
}

/* --- PROFESSIONS: who a newborn grows up to be ------------------------
 * Weighted by what the town needs and what it is for, so a mining town raises miners and
 * a threatened one raises soldiers. The profession picks the figure that gets drawn, so
 * the make-up of a crowd is legible without a single icon over anybody's head — which is
 * the lesson from the status pips that came out again. */
int mb_civ_prof_pick(int v)
{
    Village *V = &mb_v[v];
    uint32_t r = mb_rand((uint32_t)(v * 2654435761u + (uint32_t)mb_w.tick * 7717u));
    int roll = (int)(r % 100u);
    if (V->threat > 30 && roll < 25)                 return PROF_SOLDIER;
    if (mb_civ_count(v, O_TEMPLE) && roll < 32)       return PROF_PRIEST;
    if (V->tier >= TIER_TOWN && roll < 42)           return PROF_TRADER;
    if (roll < 52)                                   return PROF_BUILDER;
    if (roll < 76)                                   return (int)V->spec;   /* the town's trade */
    if (roll < 88)                                   return PROF_FARMER;
    return PROF_FORESTER;
}

/* --- CREED: what a town believes, and how it travels -----------------
 * A village founded without one takes the creed of whichever neighbour it can hear, and
 * failing that invents one. It then drifts toward whatever its kingdom's capital holds,
 * slowly, so a kingdom converges without ever being told to — and a town that disagrees
 * with its own crown is exactly where a rebellion starts.
 */
static void creed_step(int v)
{
    Village *V = &mb_v[v];
    if (!V->creed) {
        for (int u = 1; u < MAXV; u++) {
            if (u == v || !mb_v[u].alive || !mb_v[u].creed) continue;
            int dx = mb_v[u].x - V->x, dy = mb_v[u].y - V->y;
            if (dx * dx + dy * dy < 900) { V->creed = mb_v[u].creed; return; }
        }
        V->creed = (uint8_t)(1 + (mb_rand((uint32_t)(v * 40503u + (uint32_t)V->founded)) % 4u));
        return;
    }
    int k = V->kingdom;
    if (k <= 0 || k >= MAXK || !mb_k[k].alive) return;
    if (!mb_k[k].creed) { mb_k[k].creed = V->creed; return; }
    if (V->creed == mb_k[k].creed) return;
    /* Conversion is slow and it costs loyalty while it lasts — a town made to worship its
     * king's god resents it, which is what makes creed a political fact and not a label. */
    V->loyalty = (int8_t)(V->loyalty > -100 ? V->loyalty - 1 : -100);
    if ((mb_rand((uint32_t)(v * 131u + (uint32_t)mb_w.tick)) & 255) < 3)
        V->creed = mb_k[k].creed;
}


/* Somewhere to put a building: claimed, buildable, empty, and not on the hall. */
/* WHERE THE NEXT BUILDING GOES, and this is why cities used to look like stars.
 *
 * The old version walked `rad` outward from the hall and tried the EIGHT COMPASS
 * DIRECTIONS at each radius, so every building in the world landed on one of eight rays
 * from its village centre. Add a road from each building straight back to the hall and
 * you have drawn a star, every time, in every town — which is exactly what a settled
 * world looked like from God's Eye.
 *
 * Real towns are not radial, they are ACCRETIVE: a new building goes up on a street
 * that already exists, beside buildings that are already there, as close to the middle
 * as it can get. So every free cell in the claim is scored on those three things and
 * the best one wins. Frontage counts most — a house wants a road — which means the
 * town grows along its streets, and streets grow to reach new houses, and the two
 * together produce a branching fabric that thickens toward the centre. No rays.
 */
/* --- THE STREET PLAN ----------------------------------------------------
 *
 * A town is not buildings with roads threaded between them; it is STREETS with buildings
 * along them. Three versions got that the wrong way round and all three looked wrong.
 *
 *   v1 ran a spur from each finished building back to the hall, so a village was a blob
 *      of houses with stubs hanging off it.
 *   v2 pushed streets outward first, but from a RANDOM existing road cell in a RANDOM
 *      direction, so the network wandered — a tangle, not a street.
 *   v3 (this one's predecessor) kept that and added a frontage bonus to the site score.
 *      It could not work: the bonus was +40 while "closeness to the centre" was worth up
 *      to +60, so a cell in the middle of a block beat a cell on a street every time and
 *      the buildings sat in the road network's gaps.
 *
 * So the plan is DECLARED, not grown. street_at() is a pure function of the village and a
 * cell — no state, nothing to keep in sync, and it can be asked about a cell that is not
 * paved yet, which is what lets buildings know where the street WILL be:
 *
 *      - a HIGH STREET through the hall, along one axis
 *      - LANES off it every third cell, so the strips between them are exactly two deep
 *        and every buildable cell touches a lane
 *      - a second high street once the town is big enough to want two blocks
 *
 * Buildings then take the one rule that makes the fabric: never on the plan, and never
 * without a paved neighbour. Frontage stops being a preference and becomes a requirement,
 * which is the whole difference between a street with houses either side and a car park.
 *
 * It stays irregular where it matters. The axis and the lane pattern come from the village
 * id, so neighbouring towns are not copies; each lane's DEPTH is hashed, so the built-up
 * edge is ragged rather than a rectangle; and terrain masks the plan wherever it crosses
 * water, rock or an existing building, so a town on a headland is shaped by the headland.
 */
static int street_at(int v, int x, int y)
{
    const Village *V = &mb_v[v];
    int hor    = v & 1;                                 /* which way the high street runs */
    int along  = hor ? (x - V->x) : (y - V->y);
    int across = hor ? (y - V->y) : (x - V->x);
    int a = along  < 0 ? -along  : along;
    int c = across < 0 ? -across : across;

    int reach = 3 + V->pop / 2;  if (reach > 11) reach = 11;
    if (a > reach) return 0;
    if (across == 0) return 1;                           /* the high street */

    /* A SECOND high street, three cells over, once there is a town to put on it. Which
     * side it goes is the village's own, so a pair of towns do not mirror each other. */
    if (V->pop >= 20 && across == ((v & 2) ? 3 : -3)) return 1;

    /* The lanes. Depth is per-lane and hashed, so the town's outline is ragged. */
    if (along % 3) return 0;
    int deep = 2 + V->pop / 10;  if (deep > 5) deep = 5;
    int lane = 2 + (int)(mb_rand((uint32_t)(v * 2654435761u + (uint32_t)(along + 64) * 40503u))
                         % (uint32_t)(deep - 1));
    return c <= lane;
}

static int build_site(int v, int *ox, int *oy)
{
    Village *V = &mb_v[v];
    /* Is there any street yet? A village on its founding tick has none, and a rule that
     * needs frontage would deadlock before the first cell was paved. */
    int any_road = 0;
    for (int dy = -14; dy <= 14 && !any_road; dy++)
        for (int dx = -14; dx <= 14; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (mb_in(x, y) && mb_w.road[AT(x, y)] && mb_w.claim[AT(x, y)] == v) {
                any_road = 1; break;
            }
        }

    int best = -1, bx = 0, by = 0;
    for (int dy = -14; dy <= 14; dy++)
        for (int dx = -14; dx <= 14; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v) continue;
            if (!buildable(x, y) || mb_is_build(mb_w.obj[AT(x, y)])) continue;
            if (mb_w.road[AT(x, y)] || street_at(v, x, y)) continue;   /* never in the road */

            /* FRONTAGE IS A REQUIREMENT, NOT A BONUS. This one line is what makes a
             * street have houses either side of it instead of a road network with
             * buildings in its holes. */
            int front = 0;
            static const int8_t FX[4] = { 1, -1, 0, 0 }, FY[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) {
                int nx = x + FX[k], ny = y + FY[k];
                if (mb_in(nx, ny) && mb_w.road[AT(nx, ny)]) front++;
            }
            if (!front && any_road) continue;

            int nbr = 0;
            for (int k = 0; k < 8; k++) {
                static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
                static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
                int nx = x + DX[k], ny = y + DY[k];
                if (mb_in(nx, ny) && mb_is_build(mb_w.obj[AT(nx, ny)])) nbr++;
            }
            /* A CORNER PLOT IS THE BEST PLOT — two streets, so front counts double.
             * Then fill in beside what is already standing, so a street builds up along
             * its length rather than in scattered pairs. Then the centre, which is worth
             * far less than it used to be: it was outweighing frontage entirely. */
            int score = front * 24 + (nbr > 4 ? 4 : nbr) * 10;
            int d2 = dx * dx + dy * dy;
            score += (60 - d2) / 4;
            score += (int)(mb_rand((uint32_t)(AT(x, y) * 2654435761u)) & 7);

            if (score > best) { best = score; bx = x; by = y; }
        }
    if (best < 0) return 0;
    *ox = bx; *oy = by;
    return 1;
}

static int wall_site(int v, int *ox, int *oy);   /* defined below; the lord calls it */

/* THE LORD'S DECISION. One per village per visit, from a needs vector weighted
 * by the lord's stewardship. The chosen building appears as a BLUEPRINT GHOST
 * immediately and only becomes real when the stockpile pays for it — so you can
 * watch a village intend something, which is the cheapest way to make the AI
 * legible (DESIGN.md 3). */
static void lord_think(int v)
{
    Village *V = &mb_v[v];
    /* The derived facts first, because every decision below reads them. Cached on the
     * village rather than recomputed per query, so the HUD and the inspect card cost
     * nothing to draw. */
    V->tier = (uint8_t)mb_civ_tier(v);
    if ((mb_w.tick & 15) == 0) mb_civ_specialise(v);
    civic_yield(v);
    research_step(v);
    creed_step(v);
    if (V->plan_obj) return;                 /* already intending something */

    int pop = V->pop, housing = V->housing;
    /* HOW FAR AHEAD A TOWN BUILDS, and this is what lets a city exist at all. A flat
     * `pop + 8` was tuned when a world had thirty hamlets: with the world limited to one
     * settlement per sixteen souls, that same slack became the binding constraint on the
     * whole population — total beds could never exceed villages x (pop + 8), so cutting
     * the village count cut the world's carrying capacity with it and a 222-soul world
     * decayed to 54. Measured: population peaked at year 120 and halved by year 200 with
     * every death down to old age and nothing replacing it.
     *
     * A hamlet builds a little ahead; a city builds a lot. Which is both true to life and
     * the mechanism by which a successful town keeps growing instead of stalling. */
    int slack = 8 + (int)V->tier * 7;


    /* THE PRIORITY LIST, in order, as a list rather than an if-chain — because the
     * chain had a fatal shape: whichever want came first was locked in even when
     * the village could not pay for it, and try_build then waited forever. One
     * unaffordable ambition (a tier-three hall needing iron nobody had mined) stalled
     * every village in the world at six houses and one temple. A lord picks the
     * highest thing it can AFFORD, which is also just what a competent steward does.
     */
    int want_list[14], nwant = 0;

    /* A TOWN WORKS ON ITS WALL ALONGSIDE EVERYTHING ELSE. The lord takes the first
     * affordable want and stops, so anything at the BACK of the list is only ever built
     * when nothing ahead of it is wanted — and beds, bread, timber and towers are always
     * wanted. The wall sat last and was therefore never laid once, even after the gate
     * was fixed. So one build in three goes to the wall while it is unfinished: a real
     * town raises houses and curtain wall in the same decades, not one after the other. */
    /* ONE VISIT IN THREE GOES TO THE CIVIC LADDER, so a city actually modernises. */
    if (((mb_w.tick >> 5) % 3) == 0) {
        int c = civic_pick(v);
        if (c >= 0) want_list[nwant++] = (uint8_t)c;
    }

    /* A SILO GETS ITS OWN TURN, for the same reason the wall does. The lord takes the first
     * affordable want and stops, so anything at the BACK of the list is built only when
     * nothing ahead of it is wanted — and beds, bread and timber are always wanted. Put at
     * the end, the silo was never built once: measured across three 600-year worlds, two
     * kingdoms held the bomb and there were ZERO silos standing. That is the third feature
     * to die this way after the wall and the dock. */
    /* NO TURN GATE AND NO TIER GATE. Both were belt-and-braces on top of a mechanism that
     * already self-limits — a grand project becomes THE plan, and a town with a plan chooses
     * nothing else until it is paid for — and together they made silos so rare that four
     * kingdoms with the bomb produced one silo across five 900-year worlds and not a single
     * strike. If a kingdom has spent three thousand research on the bomb, its towns want the
     * thing it bought. */
    if (mb_civ_tech_ok(v, TECH_NUKE) && mb_civ_count(v, O_SILO) < 1)
        want_list[nwant++] = 22;

    int wall_turn = (V->hall >= 2 && mb_civ_count(v, O_WALL) < 44 &&
                     (V->threat > 25 || V->pop >= 14 || V->stone > 40) &&
                     ((mb_w.tick >> 6) % 3) == 0);
    if (wall_turn) want_list[nwant++] = 11;
    /* FARMS SCALE WITH MOUTHS. A flat cap of four farms fed about a dozen people, so
     * any village that grew past that starved for ever — and the audit found a world
     * that lost 2251 people to hunger while sitting at the population ceiling. Four
     * farms is a hamlet's answer; a town of forty needs seven. */
    int farm_cap = 2 + pop / 6;
    if (farm_cap > 10) farm_cap = 10;
    /* A FARMING TOWN FARMS. Specialisation earns a real bonus, not a flavour text: the
     * cap moves, so the same want list makes a granary of one village and a pithead of
     * the next without a second code path. */
    if (V->spec == PROF_FARMER) farm_cap += 3;
    if (V->food < pop * 3 && mb_civ_count(v, O_FARM) < farm_cap) want_list[nwant++] = 3;
    /* BEDS FIRST when the village is over capacity. Housing gates breeding, so a
     * village with no spare bed has no children — and with the hall and the temple
     * ahead of houses in the list, every village in a test run spent its whole
     * founding generation on civic architecture and then aged out to nothing. A
     * fancy hall is an ambition; a full house is existential. */
    /* BUT DO NOT BUILD A BED YOU CANNOT FEED. Housing gates breeding, so a lord that
     * keeps roofing an already-hungry village is building the next famine: that is
     * exactly how the 2251-death world worked, growing into the ceiling while the
     * granary emptied. Requiring a person's worth of food per head before adding beds
     * turns a runaway ramp into a village that stops growing when it runs short and
     * starts again when the harvest is in — which is why a healthy population
     * oscillates instead of climbing until it collapses. */
    /* AND NOT A HUNDRED SPARE BEDS. A village was found with 216 beds for SEVEN people:
     * houses are the cheapest thing on the ladder, so whenever nothing else was
     * affordable the lord built another one, for ever. That is most of why a town read as
     * a blob — it was mostly empty housing. A little slack for the next generation, and
     * then stop. */
    /* A FULL VILLAGE ROOFS ITSELF FIRST — but only a full one. Beds ahead of people go
     * at the BACK of the list (below), because the lord takes the first affordable want and
     * stops: with the generous tier slack in front, a growing village chose "another house"
     * every single visit and never reached its own hall. A village of sixty with a timber
     * hall in a kingdom that knew engineering is what that looks like. */
    if (pop >= housing && V->food > pop)                      want_list[nwant++] = 0;
    if (V->wood < 20 && mb_civ_count(v, O_WOODCUT) < (V->spec == PROF_FORESTER ? 4 : 2))
                                                             want_list[nwant++] = 4;
    if (V->stone < 20 && mb_civ_count(v, O_MINE) < (V->spec == PROF_MINER ? 3 : 1))
                                                             want_list[nwant++] = 5;
    /* A PORT, once the kingdom can sail and the town has water to put it on. The dock had
     * an object id, a sprite and a fuel value and was never in the build table at all. */
    /* ANY COASTAL TOWN, not only a trading one. Requiring the TRADER specialisation meant
     * that across four measured 400-year worlds only ONE ever built a quay — and a quay is
     * what lets a kingdom settle across water, so seaborne colonisation fired in one world
     * of four and looked broken. A farming town on a coast still builds a jetty. */
    if (V->coast >= 8 && mb_civ_count(v, O_DOCK) < 2)          want_list[nwant++] = 12;

    if (V->threat > 40 && mb_civ_count(v, O_BARRACKS) < 1)     want_list[nwant++] = 6;
    if (V->hall == 1)                                         want_list[nwant++] = 9;
    if (V->hall == 2)                                         want_list[nwant++] = 10;
    if (V->hall >= 2 && mb_civ_count(v, O_TEMPLE) < 1)         want_list[nwant++] = 7;
    /* and the room for the next generation, once the civic ladder has had its turn */
    if (V->food > pop * 2 && housing < pop + slack)           want_list[nwant++] = 0;
    if (V->threat > 25 && mb_civ_count(v, O_TOWER) < 3)        want_list[nwant++] = 8;
    /* A WALL, once the hall is stone and somebody is worth keeping out. Late in the list:
     * a town builds beds, bread and a barracks before it builds a curtain. */
    /* A WALL when the town is worth walling: threatened, OR simply established and
     * stocked. Gating it on threat alone meant it was never built once — threat sits at
     * zero in a peaceful world, so the condition could not fire and a feature that had a
     * sprite, an object id and a build cost had never appeared in a single game. A
     * prosperous town walls itself as a matter of course. */
    if (V->hall >= 2 && mb_civ_count(v, O_WALL) < 44 &&
        (V->threat > 25 || V->pop >= 14 || V->stone > 40))    want_list[nwant++] = 11;
    if (V->hall == 3 && housing > 0)                          want_list[nwant++] = 1;

    int want = -1;
    for (int i = 0; i < nwant; i++) {
        const BuildDef *B = &BUILD[want_list[i]];
        if (B->cap && mb_civ_count(v, B->obj) >= B->cap) continue;
        if (B->tech && !mb_civ_tech_ok(v, B->tech)) continue;   /* not yet known */
        /* A GRAND PROJECT IS COMMITTED TO, NOT AFFORDED.
         *
         * The affordability filter below is what kept the silo out of the world entirely: a
         * late-game town holds grain 600, timber 2, stone 0, iron 0, gold 0 — measured —
         * because the lord spends everything on the first affordable want every single visit.
         * So nothing expensive is ever chosen, and therefore never saved for, and three
         * kingdoms with the bomb had no silo between them.
         *
         * The blueprint ghost already solves this and was not being used for it: once a build
         * is PLANNED, try_build() waits until the stores cover it. So a grand project skips
         * the filter, becomes the plan, and the town saves — which is also the only way this
         * game can ever have a build that takes a generation. */
        if (B->grand) { want = want_list[i]; break; }
        /* Affordable now, or within a stewardship-flavoured stretch: a good steward
         * will start something it is close to paying for, a poor one will not. */
        int slack = V->lord_stew / 12;
        if (V->wood + slack < B->wood || V->stone + slack < B->stone
            || V->iron + slack < B->iron || V->gold + slack < B->gold) continue;
        want = want_list[i];
        break;
    }
    if (want < 0) return;

    const BuildDef *B = &BUILD[want];
    int x, y;
    if (B->obj == O_HALL2 || B->obj == O_HALL3) { x = V->x; y = V->y; }
    else if (B->obj == O_WALL) { if (!wall_site(v, &x, &y)) return; }
    else if (!build_site(v, &x, &y)) return;

    V->plan_obj = B->obj; V->plan_x = (uint8_t)x; V->plan_y = (uint8_t)y;
    V->plan_i = (uint8_t)want;
    if (B->obj != O_HALL2 && B->obj != O_HALL3) mb_w.obj[AT(x, y)] = O_PLAN;
}

/* Connect a new building to the STREET NETWORK — the nearest road cell, or the hall
 * if the village has no roads yet.
 *
 * Running every building's road back to the hall is the other half of the star: eight
 * rays of buildings, each with its own spoke to the middle. Joining the nearest
 * existing road instead means the second house extends the first house's street, the
 * fifth branches off it, and the network becomes a tree that grows outward — which is
 * both what real settlements do and much cheaper in paving. */
/* WHERE A WALL SEGMENT GOES: the PERIMETER of the built-up area, not the middle.
 *
 * Walls were in the object enum and had a sprite and were never once built, because they
 * were missing from the BUILD table entirely. They also cannot use build_site, which
 * scores frontage and closeness to the centre — the exact opposite of what a wall wants.
 *
 * So: take the bounding box of everything this village has built, step out one cell, and
 * walk that ring. Skip water, skip anything already standing, and SKIP ROADS — a street
 * crossing the line is a gate, and a town with no way in reads as a prison rather than a
 * fortification. Because it goes up one segment at a time through the ordinary plan-and-pay
 * machinery, you watch the wall close over years.
 */
static int wall_site(int v, int *ox, int *oy)
{
    Village *V = &mb_v[v];
    int x0 = V->x, y0 = V->y, x1 = V->x, y1 = V->y;
    for (int y = V->y - 8; y <= V->y + 8; y++)
        for (int x = V->x - 8; x <= V->x + 8; x++)
            if (mb_in(x, y) && mb_w.claim[AT(x, y)] == v && mb_is_build(mb_w.obj[AT(x, y)])) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    x0--; y0--; x1++; y1++;                       /* one clear cell outside the buildings */

    /* A WALL IS LAID AS A LINE, NOT SPRINKLED ROUND A RING.
     *
     * The ring itself was right — a bounding box a cell clear of the buildings — but the cell
     * on it was chosen AT RANDOM, so sixty stones went down as sixty unconnected posts spread
     * evenly round the perimeter. Every one of them was an isolated segment, which is why the
     * renderer had to guess an orientation for almost every cell it drew.
     *
     * So a stone prefers to go where it CONTINUES something: beside exactly one existing wall
     * extends a run, beside two closes a gap, and beside none starts a fresh one — which is
     * why a corner is worth a bonus, since a rampart that begins at a corner grows two ways
     * along the sides instead of stranding itself mid-wall. Same principle as village_streets
     * paving outward from the hall: build connected at every stage, so a half-finished thing
     * still reads as the thing.
     */
    int best = -1, bx = 0, by = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (y != y0 && y != y1 && x != x0 && x != x1) continue;   /* the ring only */
            if (!mb_in(x, y)) continue;
            int i = AT(x, y);
            if (!mb_land(mb_w.biome[i])) continue;
            /* BUILDINGS and STREETS are respected — a street crossing the line is a gate, and
             * a town with no way in reads as a prison. Everything else is scrub the masons
             * clear; refusing any cell with an object at all was why an earlier version laid
             * nothing at all, because the ring of a developed village is full of trees. */
            if (mb_is_build(mb_w.obj[i])) continue;
            if (mb_w.road[i]) continue;

            /* A WALL IS ONE STONE THICK. The ring is a bounding box round the buildings, and
             * the box GROWS as the town builds outward — so last century's rampart ends up
             * inside this century's ring and the town acquires a second wall parallel to the
             * first. Two touching rows is not a thicker wall, it is two walls: every cell then
             * has both a horizontal and a vertical neighbour, so the renderer correctly draws
             * the whole run as corner joins and a rampart comes out as a row of stone mounds.
             * Refusing to lay alongside an existing line keeps it a line. */
            int par = 0;
            if (y == y0 || y == y1) {                        /* a horizontal side */
                if (mb_in(x, y - 1) && mb_w.obj[AT(x, y - 1)] == O_WALL) par = 1;
                if (mb_in(x, y + 1) && mb_w.obj[AT(x, y + 1)] == O_WALL) par = 1;
            }
            if (x == x0 || x == x1) {                        /* a vertical side   */
                if (mb_in(x - 1, y) && mb_w.obj[AT(x - 1, y)] == O_WALL) par = 1;
                if (mb_in(x + 1, y) && mb_w.obj[AT(x + 1, y)] == O_WALL) par = 1;
            }
            if (par) continue;

            int adj = 0;
            if (mb_in(x - 1, y) && mb_w.obj[AT(x - 1, y)] == O_WALL) adj++;
            if (mb_in(x + 1, y) && mb_w.obj[AT(x + 1, y)] == O_WALL) adj++;
            if (mb_in(x, y - 1) && mb_w.obj[AT(x, y - 1)] == O_WALL) adj++;
            if (mb_in(x, y + 1) && mb_w.obj[AT(x, y + 1)] == O_WALL) adj++;

            int score = (adj == 1) ? 400            /* extend a run: much the best move */
                      : (adj == 2) ? 200            /* close a gap in one              */
                      : (adj == 0) ?  60            /* start one                       */
                                   :  10;
            if (adj == 0 && (x == x0 || x == x1) && (y == y0 || y == y1)) score += 50;
            /* a small per-town jitter, so two realms do not lay identical ramparts */
            score += (int)(mb_rand((uint32_t)(v * 977u + (uint32_t)(y * MW + x))) % 24u);
            if (score > best) { best = score; bx = x; by = y; }
        }
    if (best < 0) return 0;
    *ox = bx; *oy = by;
    return 1;
}

/* PAVE THE PLAN, nearest the hall first, a few cells a visit.
 *
 * There is no routing and no choice to make: street_at() already says where every street
 * is, so this only decides how much of it exists yet. Working outward from the hall means
 * the network is connected at every stage — a half-built plan is a short high street with
 * two lanes, which is a hamlet, and not a set of disconnected fragments.
 *
 * Terrain vetoes: water, rock and anything already standing. A plan cell that the land
 * refuses simply never gets paved, so a town on a headland is shaped by the headland
 * without a single special case.
 */
/* --- GARDENS ------------------------------------------------------------
 * The inside of a block is bare ground, and a town of houses on scraped earth looks like a
 * building site. A cell that is claimed, unpaved, unbuilt and next to a building gets planted:
 * a tree, a bush, a tuft or a flower. It costs nothing — the clutter pass already draws these
 * objects — and it is what makes a street look lived in rather than laid out.
 */
/* --- THE PLAZA ----------------------------------------------------------
 * A town's hall stands in a street like everything else, so the biggest building in the
 * world got no more space than a cottage and nowhere to be seen from. A settlement past
 * hamlet paves the block the hall is on and stands a monument in it; a city adds a fountain.
 *
 * IT PAVES RATHER THAN CLEARS. Knocking houses down to make room would be the obvious way
 * and it is the wrong one: the square would appear one day where a street used to be, and
 * whoever lived there would be homeless for reasons no player could see. Instead it only
 * takes cells that are EMPTY, so the plaza grows into the gaps and a tight town simply never
 * gets a big one — which is itself the right answer.
 */
/* A plaza cell that can take a landmark: this village's ground, buildable, and nothing
 * standing on it. Paving does not disqualify it — the square is paved first and the monument
 * then stands IN the square, which is the whole idea. */
static int plaza_free(int v, int x, int y)
{
    if (!mb_in(x, y)) return 0;
    int i = AT(x, y);
    return mb_w.claim[i] == v && buildable(x, y) && !mb_is_build(mb_w.obj[i]);
}

static void village_plaza(int v)
{
    Village *V = &mb_v[v];
    if (V->tier < TIER_TOWN) return;
    /* the hall is the anchor: find it once, near the centre */
    int hx = -1, hy = -1;
    for (int dy = -3; dy <= 3 && hx < 0; dy++)
        for (int dx = -3; dx <= 3 && hx < 0; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y)) continue;
            uint8_t o = mb_w.obj[AT(x, y)];
            if (o == O_HALL1 || o == O_HALL2 || o == O_HALL3) { hx = x; hy = y; }
        }
    if (hx < 0) return;

    /* ONE MONUMENT PER TOWN, FOUND ANYWHERE IN THE CLAIM.
     *
     * `mon` used to be set only by looking inside the 5x3 square — and the square is anchored
     * to the HALL, which MOVES when the hall upgrades to a great hall or a castle (build_site
     * picks a fresh plot). So a town that grew put up a second monument at its new square and
     * abandoned the first, which then sat in an ordinary street or a field: three monuments
     * were measured against one town. The tier that gates all this fluctuates with population
     * too, so the same thing happened to a village that crossed TOWN twice.
     *
     * Searching the whole claim costs one pass of a 23x23 box on a visit that already does
     * several, and it means a town has one monument for as long as it stands. */
    int mon = 0, fount = 0;
    for (int dy = -11; dy <= 11 && !(mon && fount); dy++)
        for (int dx = -11; dx <= 11; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v) continue;
            uint8_t o = mb_w.obj[AT(x, y)];
            if (o == O_MONUMENT) mon = 1;
            else if (o == O_FOUNTAIN) fount = 1;
        }

    /* pave what is free in a 5x3 in front of it — wider than tall, because the view is
     * three-quarters-on and a square that is square looks like a courtyard from above */
    for (int dy = 1; dy <= 3; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = hx + dx, y = hy + dy;
            if (!mb_in(x, y)) continue;
            int i = AT(x, y);
            if (mb_w.claim[i] != v || !buildable(x, y)) continue;
            uint8_t o = mb_w.obj[i];
            if (mb_is_build(o)) { if (o == O_MONUMENT) mon = 1;
                                  if (o == O_FOUNTAIN) fount = 1; continue; }
            mb_w.obj[i] = O_NONE;                       /* a tuft is not paving */
            mb_w.road[i] = 1;
        }
    /* Then the thing worth walking to. THE SPOT IS SEARCHED FOR, not fixed: the first go
     * named one cell for each (hall+2 and hall-2,+2) and if that cell happened to be a house,
     * or outside the claim, that town simply never got one — a landmark that depends on a
     * single lucky tile is a landmark that mostly does not exist. The monument takes the cell
     * nearest the middle of the square and the fountain the one furthest from it, so when a
     * town has both they are two landmarks rather than one stack. */
    if (!mon) {
        int bx = -1, by = 0, bd = 1 << 20;
        for (int dy = 1; dy <= 3; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int x = hx + dx, y = hy + dy;
                if (!plaza_free(v, x, y)) continue;
                int d = dx * dx + (dy - 2) * (dy - 2);
                if (d < bd) { bd = d; bx = x; by = y; }
            }
        if (bx >= 0) {
            mb_w.obj[AT(bx, by)] = O_MONUMENT;
            mb_w.road[AT(bx, by)] = 0;
            mb_chron_build(v, "monument");
            mon = 1;
        }
    }
    /* THE FOUNTAIN IS GATED ON TECH, NOT ON TIER. It was TIER_CITY, which wants pop 34 in one
     * settlement — a world caps around 222 souls across ten of them, so in six hundred years of
     * five worlds not one fountain was ever built. A fountain needs a water supply, so the
     * honest gate is the engineering that makes one, and that a kingdom really does reach. */
    if (!fount && mon && mb_civ_tech_ok(v, TECH_ENGINEER)) {
        int bx = -1, by = 0, bd = -1;
        for (int dy = 1; dy <= 3; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int x = hx + dx, y = hy + dy;
                if (!plaza_free(v, x, y)) continue;
                int d = dx * dx + (dy - 2) * (dy - 2);
                if (d > bd) { bd = d; bx = x; by = y; }
            }
        if (bx >= 0) {
            mb_w.obj[AT(bx, by)] = O_FOUNTAIN;
            mb_w.road[AT(bx, by)] = 0;
            mb_chron_build(v, "fountain");
        }
    }
}

static void village_gardens(int v)
{
    Village *V = &mb_v[v];
    for (int pass = 0; pass < 4; pass++) {
        uint32_t r = mb_rand((uint32_t)(v * 6151u + (uint32_t)mb_w.tick * 131u + pass));
        int x = V->x + (int)(r % 19u) - 9, y = V->y + (int)((r >> 8) % 19u) - 9;
        if (!mb_in(x, y)) continue;
        int i = AT(x, y);
        if (mb_w.claim[i] != v || mb_w.road[i] || mb_w.obj[i]) continue;
        if (!buildable(x, y)) continue;
        if (street_at(v, x, y)) continue;              /* never plant in a future street */
        int near = 0;
        for (int k = 0; k < 8 && !near; k++) {
            static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
            static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
            int nx = x + DX[k], ny = y + DY[k];
            if (mb_in(nx, ny) && mb_is_build(mb_w.obj[AT(nx, ny)])) near = 1;
        }
        if (!near) continue;                            /* a garden belongs to a building */
        int pick = (int)((r >> 20) & 7);
        mb_w.obj[i] = (pick < 3) ? O_TUFT : (pick < 5) ? O_BUSH
                    : (pick < 7) ? O_FLOWER : O_TREE;
    }
}

static void village_streets(int v)
{
    Village *V = &mb_v[v];

    /* STREET IS PAVED ON DEMAND, and this is the rule that was missing. The plan is laid
     * out for a finished town, but paving it at three cells a visit while buildings wait
     * on a stockpile meant the road always won the race: the first screenshots of this
     * layout were a complete empty grid with a dozen houses lost in it — a car park with
     * a village in the corner.
     *
     * So the network may only be as big as there is town to line it: two street cells per
     * building, plus a few to get started. Blocks therefore FILL before new lanes open,
     * which is also the order a real settlement does it in. */
    int built = 0, paved = 0;
    for (int dy = -14; dy <= 14; dy++)
        for (int dx = -14; dx <= 14; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v) continue;
            if (mb_w.road[AT(x, y)]) paved++;
            if (mb_is_build(mb_w.obj[AT(x, y)])) built++;
        }
    /* One street cell per building. A lane cell of a two-deep block fronts four
     * plots, so this is still generous — at two per building the grid outran the
     * town and the first screenshots were an empty car park. */
    if (paved >= 3 + built) return;

    /* nearest unpaved plan cell to the hall, so the network is connected at every stage */
    int best = -1, bx = 0, by = 0;
    for (int dy = -14; dy <= 14; dy++)
        for (int dx = -14; dx <= 14; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y)) continue;
            int i = AT(x, y);
            if (mb_w.road[i] || mb_w.claim[i] != v) continue;
            if (!street_at(v, x, y)) continue;
            if (!buildable(x, y) || mb_is_build(mb_w.obj[i])) continue;
            int d = dx * dx + dy * dy;
            if (best < 0 || d < best) { best = d; bx = x; by = y; }
        }
    if (best < 0) return;
    int i = AT(bx, by);
    mb_w.road[i] = 1;
    if (mb_w.obj[i] && !mb_is_build(mb_w.obj[i])) mb_w.obj[i] = O_NONE;      /* clear scrub */
}


/* --- SUCCESSION: a town's fortunes turn on who is running it -----------
 * The lord had three stats and a trait roll and then lived for ever, so a village's
 * character was fixed on its founding tick — a well-stewarded town stayed well stewarded
 * for five hundred years and a badly led one never got a second chance. That is the
 * opposite of what makes a history worth reading.
 *
 * So lords age and die, and the heir is their own child: stats near the parent's but not
 * equal, traits mostly inherited. A dynasty therefore has a character that drifts, an
 * ambitious line stays ambitious, and the death of a good lord is genuinely bad news for
 * the town — which is a thing you can watch happen in the chronicle.
 */
static void succession_step(int v)
{
    Village *V = &mb_v[v];
    if ((mb_w.tick % 52)) return;                      /* once a year: 52 ticks */
    V->lord_age++;
    if (V->lord_age < 58) return;
    if ((mb_rand((uint32_t)(v * 7717u + (uint32_t)mb_w.tick)) & 7) > 2) return;

    uint32_t r = mb_rand((uint32_t)(v * 2654435761u + (uint32_t)mb_w.tick * 131u));
    /* the heir: near the parent, drifting, with a real chance of being a disappointment */
    #define HEIR(stat) do {                                                        \
        int base = (int)V->stat, d = (int)(r % 41u) - 20;                           \
        r = mb_rand(r);                                                             \
        base += d; V->stat = (uint8_t)(base < 0 ? 0 : base > 99 ? 99 : base);        \
    } while (0)
    HEIR(lord_diplo); HEIR(lord_stew); HEIR(lord_war);
    #undef HEIR
    /* traits pass down, and one in four mutates — the same rule bloodlines use */
    if ((r & 3) == 0) V->lord_traits ^= TR_AMBITIOUS;
    if (((r >> 2) & 7) == 0) V->lord_traits ^= TR_LOYAL;
    V->lord_name = mb_name_person(r ^ (uint32_t)mb_w.tick);
    V->lord_age = (uint8_t)(19 + (r >> 8) % 16u);
    /* a new lord is untested: the town holds its breath */
    V->loyalty = (int8_t)(V->loyalty > 20 ? V->loyalty - 15 : V->loyalty);
    mb_chron_age(MB_TIER_NAME[V->tier]);
}

/* --- WHERE A FIELD GOES ---------------------------------------------------
 *
 * Fields were sited with build_site() — the HOUSE-PLOT picker, which by design requires street
 * frontage. So farmland was sown on exactly the plots the town was about to build on, and
 * housing won: measured across a run, 24 farms and 163 field cells at year 60 collapsing to 10
 * and 51 by year 200. The fields were not failing to appear, they were being built over.
 *
 * A field wants the opposite of a building plot: OFF the street plan, out toward the edge of
 * the claim, and next to other fields so the land reads as a belt of worked ground rather than
 * as green confetti. Scrub is cleared to sow — that is what clearing land IS — but nothing
 * built is ever touched.
 */
static int field_site(int v, int *ox, int *oy)
{
    Village *V = &mb_v[v];
    int best = -1, bx = 0, by = 0;
    for (int dy = -11; dy <= 11; dy++)
        for (int dx = -11; dx <= 11; dx++) {
            int x = V->x + dx, y = V->y + dy;
            if (!mb_in(x, y)) continue;
            int i = AT(x, y);
            if (mb_w.claim[i] != v || mb_w.road[i]) continue;
            if (mb_is_build(mb_w.obj[i])) continue;
            if (!buildable(x, y)) continue;
            if (street_at(v, x, y)) continue;          /* never on a future street */
            uint8_t b = mb_w.biome[i];
            if (b == B_FARM) continue;                 /* already sown */
            if (b != B_GRASS && b != B_MEADOW && b != B_SAVANNA) continue;   /* soil only */
            /* next to worked land first, then the outskirts: a belt, growing outward */
            int adj = 0;
            if (x > 0      && mb_w.biome[i - 1]  == B_FARM) adj++;
            if (x < MW - 1 && mb_w.biome[i + 1]  == B_FARM) adj++;
            if (y > 0      && mb_w.biome[i - MW] == B_FARM) adj++;
            if (y < MH - 1 && mb_w.biome[i + MW] == B_FARM) adj++;
            /* ADJACENCY DOMINATES. At adj*60 the distance term (up to 242 at this radius)
             * outweighed a full set of worked neighbours, so fields scattered along the rim of
             * the claim as green confetti instead of joining up. Worked land wants to be a
             * BLOCK — that is what a field is — so sharing an edge with one is worth more than
             * any amount of being far out. */
            int score = adj * 400 + dx * dx + dy * dy;
            if (score > best) { best = score; bx = x; by = y; }
        }
    if (best < 0) return 0;
    *ox = bx; *oy = by;
    return 1;
}

/* --- CARAVANS: a surplus in one town is a shortage answered in another -
 * Roads existed and carried nobody. Nothing ever moved between settlements, so a famine
 * was always local, a rich town's granary did nothing for its neighbour, and the road
 * network — the most expensive thing a village builds — had no purpose beyond decoration.
 *
 * A town with a real surplus and a neighbour in real need sends one of its own away with
 * a load. The hauler walks, on foot, visibly, and can be killed on the way; when it
 * arrives the goods change hands and the receiving town remembers it. Which means:
 *   - roads earn their existence, and there are figures moving along them
 *   - famine becomes a regional event with a chance of rescue
 *   - a raider or a fire on a trade road has consequences two towns away
 *   - a specialised town can survive on one trade, because the rest arrives
 */
static void caravan_step(int v)
{
    Village *V = &mb_v[v];
    if (V->tier < TIER_HAMLET || V->pop < 5) return;
    /* THINNED AGAINST THE VISIT SCHEDULE, not against absolute time. The first version
     * required (tick % 24) == 0 as well — but a village is only VISITED once per rotation,
     * so the two conditions coincided about once every 1128 ticks per village and the whole
     * system fired ONCE in four hundred years. MOTEBOX_LOOPS caught it; nothing else could
     * have. Thinning has to be expressed in VISITS, so this is one village in three per
     * rotation, which makes a caravan an occasional event rather than a heartbeat. */
    /* A STATION EARNS THE RAILWAY. TECH_RAILWAY gated the station and the station did
     * nothing at all, so an industrial kingdom's freight moved at the speed of a stone-age
     * hamlet's. With a station a town despatches every other rotation instead of every third,
     * carries a bigger load, and will ship METAL as well as food and timber — which is the
     * only way iron and gold ever reach a town that has none in its own ground. */
    int rail = mb_civ_count(v, O_STATION) > 0;
    if (((mb_w.tick / 47) + (uint32_t)v) % (rail ? 2 : 3)) return;

    /* what we can spare, and how badly */
    int kind = -1, amount = 0;
    if (V->food > V->pop * 5 + 20) { kind = CARRY_FOOD;  amount = 12; }
    else if (V->wood > 60)         { kind = CARRY_WOOD;  amount = 10; }
    else if (V->stone > 60)        { kind = CARRY_STONE; amount = 10; }
    else if (rail && V->iron > 40) { kind = CARRY_IRON;  amount = 8; }
    else if (rail && V->gold > 40) { kind = CARRY_GOLD;  amount = 8; }
    else return;
    if (rail) amount += 6;

    /* the neediest reachable neighbour of the same kingdom, or an ally */
    int best = -1, bestneed = 0;
    for (int u = 1; u < MAXV; u++) {
        if (u == v || !mb_v[u].alive) continue;
        int dx = mb_v[u].x - V->x, dy = mb_v[u].y - V->y;
        int d2 = dx * dx + dy * dy;
        if (d2 > (rail ? 4900 : 1600)) continue;       /* forty cells on foot, seventy by rail */
        int friendly = (mb_v[u].kingdom == V->kingdom) ||
                       (V->kingdom && (mb_k[V->kingdom].ally_with & (1u << mb_v[u].kingdom)));
        if (!friendly) continue;
        int need = 0;
        if (kind == CARRY_FOOD)       need = mb_v[u].pop * 3 - (int)mb_v[u].food;
        else if (kind == CARRY_WOOD)  need = 30 - (int)mb_v[u].wood;
        else if (kind == CARRY_IRON)  need = 30 - (int)mb_v[u].iron;
        else if (kind == CARRY_GOLD)  need = 30 - (int)mb_v[u].gold;
        else                          need = 30 - (int)mb_v[u].stone;
        if (need > bestneed) { bestneed = need; best = u; }
    }
    if (best < 0) return;

    /* draft a trader if the town has one, else anybody idle — a trading town moves goods
     * more readily than a farming one, which is what the profession is FOR */
    int who = -1;
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || u->sp >= SP_CIV_N || u->village != v) continue;
        if (u->job == JOB_FLEE || u->job == JOB_FIGHT || u->job == JOB_HAUL) continue;
        if (u->prof == PROF_TRADER) { who = i; break; }
        if (who < 0) who = i;
    }
    if (who < 0) return;

    Unit *h = &mb_u[who];
    h->job = JOB_HAUL;
    h->dest = (uint8_t)best;
    h->carry = (uint8_t)amount;
    h->carry_kind = (uint8_t)kind;
    h->target = (uint16_t)AT(mb_v[best].x, mb_v[best].y);
    if (kind == CARRY_FOOD)       V->food  = (uint16_t)(V->food  - amount);
    else if (kind == CARRY_WOOD)  V->wood  = (uint16_t)(V->wood  - amount);
    else if (kind == CARRY_IRON)  V->iron  = (uint16_t)(V->iron  - amount);
    else if (kind == CARRY_GOLD)  V->gold  = (uint16_t)(V->gold  - amount);
    else                          V->stone = (uint16_t)(V->stone - amount);
}

/* Called by the unit brain when a hauler reaches its destination. */
void mb_civ_deliver(int v, int kind, int amount)
{
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) return;
    Village *V = &mb_v[v];
    if (kind == CARRY_FOOD)       V->food  = (uint16_t)(V->food  + amount);
    else if (kind == CARRY_WOOD)  V->wood  = (uint16_t)(V->wood  + amount);
    else if (kind == CARRY_STONE) V->stone = (uint16_t)(V->stone + amount);
    else if (kind == CARRY_IRON)  V->iron  = (uint16_t)(V->iron  + amount);
    else                          V->gold  = (uint16_t)(V->gold  + amount);
    V->traded++;
    V->happy = (int8_t)(V->happy < 120 ? V->happy + 2 : V->happy);
}

/* Pay for the plan if the store allows, and raise the building. */
static void try_build(int v)
{
    Village *V = &mb_v[v];
    if (!V->plan_obj) return;
    const BuildDef *B = &BUILD[V->plan_i];
    if (V->wood < B->wood || V->stone < B->stone || V->iron < B->iron || V->gold < B->gold)
        return;
    V->wood -= B->wood; V->stone -= B->stone; V->iron -= B->iron; V->gold -= B->gold;
    mb_w.obj[AT(V->plan_x, V->plan_y)] = V->plan_obj;
    /* NO SPUR ROAD. Every finished building used to run an L back to the hall, which is
     * where the stubs and the tangle came from. The street plan is connected by
     * construction and the building had to have frontage to be sited at all, so it is
     * already on a street: there is nothing left to connect. */
    if (V->plan_obj == O_HALL2) V->hall = 2;
    if (V->plan_obj == O_HALL3) V->hall = 3;
    V->dirty = 1;
    mb_chron_build(v, B->name);
    V->plan_obj = 0;
}

/* --- what a village asks of its people ---------------------------------- */

int mb_village_need(int v, uint16_t *target)
{
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) return 0;
    Village *V = &mb_v[v];
    /* the scarcest thing wins, so labour follows shortage without a scheduler */
    /* WHY EVERYBODY WAS IDLE. A job census found four fifths of every population doing
     * nothing at every point in history — 180 of 222 at year 100, 159 of 222 at year 600 — and
     * the reason is all three of these lines.
     *
     * The food want fired only below pop*3 and then asked for `60 - food`, which is NEGATIVE
     * for any town holding sixty or more: so a hungry town of thirty (needing ninety) wanted
     * nothing at all. Wood capped at forty and stone at thirty, which a village fills in its
     * first decade and never falls below again. All three wants sat at zero for ever, the
     * utility brain found nothing worth doing, and two hundred people milled about the square.
     *
     * Wants scale with the MOUTHS and the WORK now, and none of them can go negative. A town
     * always has something worth doing, which is the whole point of having people in it. */
    int need_food  = V->pop * 8 - (int)V->food;
    /* AND THE BELT ITSELF IS WORK. Food used to be wanted only in proportion to how empty
     * the granary was, so a town with a full barn wanted nothing from its fields — the crop
     * stood uncut and the bare ground stayed bare, which is the whole difference between a
     * farm that is farmed and a texture that changes colour. A standing harvest is worth
     * fetching at any store level, and next year's sowing is worth doing today. */
    if (V->ripe   * 7 > need_food) need_food = V->ripe   * 7;
    if (V->fallow * 3 > need_food) need_food = V->fallow * 3;
    int need_wood  = 90 - (int)V->wood;
    int need_stone = 70 - (int)V->stone;
    if (need_food  < 0) need_food  = 0;
    if (need_wood  < 0) need_wood  = 0;
    if (need_stone < 0) need_stone = 0;
    /* IRON AND GOLD, WANTED FOR A REASON. The gates were `hall >= 2` for iron and
     * `hall >= 3` for gold — and a tier-three hall COSTS gold, so a village could not want
     * gold until it already had the building that gold pays for. Nothing in any run ever
     * mined a nugget (the inspect dump read `iron 0, gold 0` at year 600), and every
     * expensive project had to be exempted from the affordability test to get planned at all.
     *
     * The honest gate is the BLUEPRINT. Whatever the town has decided to build, it fetches
     * what that costs — so a silo makes miners of a whole valley, and a town with nothing
     * planned does not send people up a mountain for no reason. */
    int need_iron = 0, need_gold = 0;
    if (V->plan_obj && V->plan_i < NBUILD) {
        int short_iron = (int)BUILD[V->plan_i].iron - (int)V->iron;
        int short_gold = (int)BUILD[V->plan_i].gold - (int)V->gold;
        if (short_iron > 0) need_iron = 30 + short_iron;
        if (short_gold > 0) need_gold = 26 + short_gold;
    }
    int best = need_food, kind = CARRY_FOOD;
    if (need_wood > best)  { best = need_wood;  kind = CARRY_WOOD; }
    if (need_stone > best) { best = need_stone; kind = CARRY_STONE; }
    if (need_iron > best)  { best = need_iron;  kind = CARRY_IRON; }
    if (need_gold > best)  { best = need_gold;  kind = CARRY_GOLD; }
    if (best <= 0) return 0;
    /* the target is a resource cell of that kind near the village */
    int x, y;
    if (!mb_village_resource(v, kind, &x, &y)) return 0;
    *target = (uint16_t)AT(x, y);
    return best / 2 + 20;
}

/* Find the nearest thing of a kind worth walking to. Deliberately a bounded
 * spiral rather than a search over the map: a villager's world is its valley. */
int mb_village_resource(int v, int kind, int *ox, int *oy)
{
    Village *V = &mb_v[v];
    /* SPREAD THE LABOUR. The spiral returned the first hit in a fixed scan order, so every
     * villager who wanted the same thing was sent to the same cell — one tree felled by a
     * queue of nine while the wood behind it stood, and one field cell worked over and over
     * while the rest of the belt lay unsown. A rotating skip hands consecutive callers
     * different cells for the price of one byte, and the first hit is kept as a fallback so a
     * valley with a single tree left in it still gets pointed at that tree. */
    static uint8_t s_spread;
    int skip = (int)(s_spread++ & 3u), fx = -1, fy = -1;
    for (int rad = 1; rad < 14; rad++)
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dy * dy < (rad - 1) * (rad - 1)) continue;
                int x = V->x + dx, y = V->y + dy;
                if (!mb_in(x, y)) continue;
                uint8_t o = mb_w.obj[AT(x, y)], b = mb_w.biome[AT(x, y)];
                int hit = 0;
                if (kind == CARRY_WOOD)  hit = (o == O_TREE || o == O_TREE2 || o == O_DEAD);
                if (kind == CARRY_STONE) hit = (o == O_ROCK || o == O_BOULDER || b == B_MOUNTAIN || b == B_HILL);
                if (kind == CARRY_IRON)  hit = (o == O_ORE);
                if (kind == CARRY_GOLD)  hit = (o == O_GOLD || o == O_GEM || o == O_SILVER);
                /* A GROWING CROP IS NOT A TARGET. `b == B_FARM` matched a field at any stage,
                 * so villagers walked into standing wheat and took seven food out of it. Two
                 * things on a field are worth the walk: a ripe crop to cut, and bare ground to
                 * sow. Which of the two it is, is decided on arrival by what is actually there. */
                if (kind == CARRY_FOOD)  hit = (o == O_BUSH || o == O_FLOWER ||
                                                (b == B_FARM && (o == O_RIPE || o == O_NONE)));
                if (hit) {
                    if (fx < 0) { fx = x; fy = y; }
                    if (skip-- <= 0) { *ox = x; *oy = y; return 1; }
                }
            }
    if (fx >= 0) { *ox = fx; *oy = fy; return 1; }
    return 0;
}

/* A working villager: walk to the target, take what is there, walk it home. */
void mb_village_work(int v, int ui)
{
    Unit *u = &mb_u[ui];
    if (v <= 0 || v >= MAXV || !mb_v[v].alive) { u->job = JOB_IDLE; return; }
    Village *V = &mb_v[v];
    int x = u->x >> 4, y = u->y >> 4;

    if (u->carry) {                                    /* heading home */
        int hx, hy;
        if ((x == V->x && y == V->y) ||
            ((V->x - x) * (V->x - x) + (V->y - y) * (V->y - y) <= 2)) {
            switch (u->carry_kind) {
            case CARRY_WOOD:  V->wood  += u->carry; break;
            case CARRY_STONE: V->stone += u->carry; break;
            case CARRY_IRON:  V->iron  += u->carry; break;
            case CARRY_GOLD:  V->gold  += u->carry; break;
            default:          V->food  += u->carry; break;
            }
            u->carry = 0; u->job = JOB_IDLE; u->happy += 2;
            return;
        }
        if (mb_village_step_home(v, x, y, &hx, &hy)) {
            /* one gradient step, converted back to a move target */
            u->target = (uint16_t)AT(hx, hy);
        } else u->target = (uint16_t)AT(V->x, V->y);
        int tx = u->target % MW, ty = u->target / MW;
        int spd = MB_SP[u->sp].speed;
        int dx = tx * 16 + 8 - u->x, dy = ty * 16 + 8 - u->y;
        u->x = (uint16_t)(u->x + (dx > spd ? spd : (dx < -spd ? -spd : dx)));
        u->y = (uint16_t)(u->y + (dy > spd ? spd : (dy < -spd ? -spd : dy)));
        return;
    }

    if (u->target == 0xFFFF) { u->job = JOB_IDLE; return; }
    int tx = u->target % MW, ty = u->target / MW;
    if ((tx - x) * (tx - x) + (ty - y) * (ty - y) <= 2) {          /* arrived */
        uint8_t *o = &mb_w.obj[AT(tx, ty)];
        uint8_t b = mb_w.biome[AT(tx, ty)];
        if (*o == O_TREE || *o == O_TREE2 || *o == O_DEAD) { *o = O_NONE; u->carry = 8; u->carry_kind = CARRY_WOOD; }
        else if (*o == O_ROCK || *o == O_BOULDER)          { *o = O_NONE; u->carry = 8; u->carry_kind = CARRY_STONE; }
        /* A SEAM IS WORKED, NOT PICKED UP. Trees and loose rock are taken away and the cell
         * is emptied, which is right; doing the same to an ore body meant a valley was mined
         * out for ever after a handful of trips, so the one income that depends on terrain
         * destroyed its own terrain. The deposit stays. */
        else if (*o == O_ORE)                              { u->carry = 6; u->carry_kind = CARRY_IRON; }
        else if (*o == O_GOLD || *o == O_SILVER || *o == O_GEM) { u->carry = 5; u->carry_kind = CARRY_GOLD; }
        else if (*o == O_BUSH || *o == O_FLOWER)           { *o = O_NONE; u->carry = 6; u->carry_kind = CARRY_FOOD; }
        else if (b == B_MOUNTAIN || b == B_HILL)           { u->carry = 5; u->carry_kind = CARRY_STONE; }
        /* THE FIELD IS SOWN AND CUT BY HAND. This paid seven food for standing on any farm
         * cell whatever was growing on it, which is why the whole belt ripened and was cut on
         * one world tick with the farmers as bystanders. A farmer now finds either bare
         * ground, and sows it, or a ripe crop, and carries it in. */
        else if (b == B_FARM) {
            if (*o == O_RIPE)      { *o = O_NONE; u->carry = 24; u->carry_kind = CARRY_FOOD; }
            else if (*o == O_NONE) { *o = O_SOWN; u->happy += 1;
                                     u->target = 0xFFFF; return; }   /* sown, and home empty-handed */
            else { u->target = 0xFFFF; u->job = JOB_IDLE; return; }
        }
        else { u->target = 0xFFFF; u->job = JOB_IDLE; return; }
        u->target = 0xFFFF;
        return;
    }
    /* walk to it */
    int spd = MB_SP[u->sp].speed;
    int dx = tx * 16 + 8 - u->x, dy = ty * 16 + 8 - u->y;
    int nx = u->x + (dx > spd ? spd : (dx < -spd ? -spd : dx));
    int ny = u->y + (dy > spd ? spd : (dy < -spd ? -spd : dy));
    if (mb_unit_passable(u->sp, nx >> 4, ny >> 4)) { u->x = (uint16_t)nx; u->y = (uint16_t)ny; }
    else u->target = 0xFFFF;
}

int mb_village_mustering(int v)
{
    return (v > 0 && v < MAXV && mb_v[v].alive) ? mb_v[v].mustering : 0;
}

/* --- the village tick --------------------------------------------------- */

/* Census: population, housing, mood and threat, gathered from the units and the
 * map rather than tracked incrementally — an incremental count drifts the moment
 * anything kills a unit outside the village's own code, and this is 384 reads. */
/* HOMELESS PEOPLE MUST FIND A HOME.
 *
 * When a village dies its citizens are set adrift (village = 0) and nothing ever
 * picked them up again. They then had no granary — a villager eats from the village
 * store, and a person with no village can only find a bush — but they kept breeding,
 * because the housing gate that limits a village's growth does not apply to somebody
 * who has no housing to be gated by. The audit found a world running 400 years with
 * 152 HOMELESS DWARVES: an entire people with no settlement anywhere, multiplying and
 * starving, and starvation was a third of all deaths in that world.
 *
 * A people that loses its town does one of two things, so this does both: walk to the
 * nearest one within a day's travel and join it, or, if there is nothing to walk to,
 * stop and found a new one. Either way the population is answerable to housing and
 * food again, which is what makes the curve level off instead of ramping into famine.
 */
void mb_civ_rehome(void)
{
    /* a slice per tick — this is a search per unit, and nobody's homelessness is
     * urgent enough to pay for it every week */
    for (int i = (int)(mb_w.tick & 15); i < mb_nu; i += 16) {
        Unit *u = &mb_u[i];
        if (!u->alive || u->sp >= SP_CIV_N || u->village) continue;
        int ux = u->x >> 4, uy = u->y >> 4;

        int best = 0, bestd = 21 * 21;
        for (int v = 1; v < MAXV; v++) {
            if (!mb_v[v].alive) continue;
            int dx = mb_v[v].x - ux, dy = mb_v[v].y - uy;
            int d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; best = v; }
        }
        if (best) { u->village = (uint8_t)best; continue; }

        /* Nothing within reach. Found one where they stand — but only if this is
         * ground worth settling, and only for one wanderer per tick, or a scattered
         * people would sprout forty hamlets in a single week. */
        if ((mb_w.tick & 31) != 0) continue;
        if (!mb_in(ux, uy) || !mb_land(mb_w.biome[AT(ux, uy)])) continue;
        if (mb_w.biome[AT(ux, uy)] == B_LAVA || mb_w.biome[AT(ux, uy)] == B_ACID) continue;
        int v = mb_village_found(u->sp, ux, uy, 0);
        if (v) {
            mb_v[v].food = 20; mb_v[v].wood = 8;
            u->village = (uint8_t)v;
            /* everyone else nearby joins the new settlement */
            for (int j = 0; j < mb_nu; j++) {
                Unit *w = &mb_u[j];
                if (!w->alive || w->sp >= SP_CIV_N || w->village) continue;
                int dx = (w->x >> 4) - ux, dy = (w->y >> 4) - uy;
                if (dx * dx + dy * dy <= 12 * 12) w->village = (uint8_t)v;
            }
            mb_chron_found(v);
        }
    }
}


static void census(void)
{
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive) continue;
        mb_v[v].pop = 0; mb_v[v].happy = 0; mb_v[v].soldiers = 0;
    }
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || !u->village || u->village >= MAXV) continue;
        Village *V = &mb_v[u->village];
        if (!V->alive) { u->village = 0; continue; }
        V->pop++;
        V->happy = (int8_t)((V->happy * 3 + u->happy) / 4);
        if (u->job == JOB_FIGHT) V->soldiers++;
    }
    mb_civ_rehome();

    for (int v = 1; v < MAXV; v++) {
        Village *V = &mb_v[v];
        if (!V->alive) continue;
        V->housing = 0;
        for (int y = V->y - 8; y <= V->y + 8; y++)
            for (int x = V->x - 8; x <= V->x + 8; x++) {
                if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v) continue;
                uint8_t o = mb_w.obj[AT(x, y)];
                if (o == O_HOUSE1) V->housing += 3;
                else if (o == O_HOUSE2) V->housing += 5;
                else if (o == O_HOUSE3) V->housing += 8;
            }
        V->housing += 4;                     /* the hall itself sleeps a few */
    }
}

/* Claim creep: a village slowly annexes the unclaimed land around it, which is
 * what draws the political map in God's Eye without a border algorithm. */
static void claim_creep(int v)
{
    Village *V = &mb_v[v];
    int reach = 4 + V->hall * 2 + (V->pop > 20 ? 2 : 0);
    uint32_t r = mb_rand((uint32_t)(v * 7717u));
    for (int t = 0; t < 6; t++) {
        int dx = (int)((r >> (t * 4)) % (uint32_t)(reach * 2 + 1)) - reach;
        int dy = (int)((r >> (t * 4 + 2)) % (uint32_t)(reach * 2 + 1)) - reach;
        int x = V->x + dx, y = V->y + dy;
        if (!mb_in(x, y) || mb_w.claim[AT(x, y)]) continue;
        if (!mb_land(mb_w.biome[AT(x, y)])) continue;
        mb_w.claim[AT(x, y)] = (uint8_t)v;
    }
}

/* Settlers: a full village sends a party out to found another. This is how a
 * single spawn becomes a civilisation, and it is why the world fills in. */
static void maybe_settle(int v)
{
    Village *V = &mb_v[v];
    /* SPLITTING IS THE ENEMY OF A CITY. At a threshold of 18 the world came out as
     * twenty-nine hamlets sharing 222 people — seven each — so no settlement ever reached
     * town, let alone city, and the whole building ladder above a stone hall was dead
     * content. Measured with MOTEBOX_LOOPS: hamlet=22, village=5, TOWN=1, CITY=0.
     *
     * A colony now costs a real town: a full quarter of the world's people cannot be
     * living in hamlets that are each about to found another one. So: it must be at least
     * a proper village, it must be genuinely CROWDED rather than merely populous, and it
     * must have the stores to send people away with. */
    if (V->tier < TIER_VILLAGE) return;
    if (V->pop < 22) return;
    if (V->food < 120) return;
    if (mb_village_count() >= MAXV - 1) return;
    /* AND A WORLD-LEVEL LIMIT, which is the rule that actually works. Per-village tests
     * cannot stop fragmentation: each village looks reasonable on its own while the world
     * ends up as twenty-nine hamlets of seven people. So the world may carry one settlement
     * per sixteen living souls, and no more. Under that, a growing population founds towns
     * and a static one grows the towns it has.
     *
     * (An earlier attempt required `pop < housing` as a crowding test and had it backwards:
     * housing is deliberately built AHEAD of population, so the condition was almost always
     * true, settling stopped dead, and the world fell to three villages and 37 people.
     * Measured, not guessed — MOTEBOX_LOOPS again.) */
    {
        int souls = 0;
        for (int i = 0; i < mb_nu; i++)
            if (mb_u[i].alive && mb_u[i].sp < SP_CIV_N) souls++;
        /* ONE SETTLEMENT PER TWENTY-FOUR SOULS. At sixteen the world came out as
         * thirteen villages of seventeen — every one stuck below the twenty-population
         * threshold for a town, so TOWN and CITY and everything gated on them never
         * appeared. Fewer, bigger. */
        if (mb_village_count() * 24 > souls) return;
    }
    /* A COOLDOWN, not a lottery. At 1-in-64 per visit and one visit per 47 ticks,
     * the expected wait between settling attempts was three thousand ticks — sixty
     * years — so a thriving village never colonised anything and the world stayed
     * a single village however well it did. */
    if (mb_w.tick - V->last_settle < 400) return;   /* eight years between colonies */
    V->last_settle = mb_w.tick;
    uint32_t r = mb_rand((uint32_t)(v * 6151u + 3u + (uint32_t)mb_w.tick));

    /* CAN THIS TOWN PUT TO SEA? A kingdom that knows seafaring, and a town with a quay to
     * sail from, settles ACROSS WATER — which is the one system that changes the shape of a
     * whole world rather than the shape of a town. Islands were unreachable before: a
     * continent filled up and stopped, and every archipelago world stayed empty forever
     * however well its one landmass did.
     *
     * Colonists sail rather than walk, so they are put ashore at the new hall instead of
     * pathing. That is honest — they took ship, and a walking route over open ocean is the
     * thing that does not exist — and it is visible in the right way: a dock goes up, and
     * some years later there is a village on the far island flying the same banner. */
    int seaborne = mb_civ_tech_ok(v, TECH_SEAFARING) && mb_civ_count(v, O_DOCK) > 0;

    /* look for a site a comfortable distance out: far enough to be its own place, near
     * enough that the parent's people can get there. A seafaring town reaches much
     * further, because a ship does not care what is in between. */
    for (int t = 0; t < 16; t++) {
        int ang = (int)((r >> (t + 4)) & 7);
        static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        int d = 10 + (int)((r >> (t + 8)) & 7);
        /* NAVIGATION was a leaf: a kingdom could research it and nothing whatever changed.
         * Seafaring gets you off the beach; navigation is knowing where you are once you are
         * out there, so it reaches further and it tries the sea on EVERY heading rather than
         * every other one — which in an archipelago world is the difference between filling
         * the near islands and filling the map. */
        int nav = mb_civ_tech_ok(v, TECH_NAVIGATION);
        if (seaborne && ((t & 1) || nav)) d += 12 + (int)((r >> t) & 15) + (nav ? 14 : 0);
        int x = V->x + DX[ang] * d, y = V->y + DY[ang] * d;
        if (mb_village_found(V->sp, x, y, V->kingdom)) {
            int nv = mb_village_count_last();
#if MOTE_HOST
            if (seaborne) {
                extern int mb_seaborne_colonies;
                mb_seaborne_colonies++;
                fprintf(stderr, "seaborne colony: %s -> %d,%d (%d cells out)\n",
                        "ship", x, y, d);
            }
#endif
            V->food -= 60;
            /* four colonists change allegiance; if they sailed, they arrive */
            int moved = 0;
            for (int i = 0; i < mb_nu && moved < 4; i++)
                if (mb_u[i].alive && mb_u[i].village == v) {
                    mb_u[i].village = (uint8_t)nv;
                    mb_u[i].job = JOB_IDLE; mb_u[i].target = 0xFFFF;
                    if (seaborne) {
                        mb_u[i].x = (uint16_t)(mb_v[nv].x * 16 + 8);
                        mb_u[i].y = (uint16_t)(mb_v[nv].y * 16 + 8);
                    }
                    moved++;
                }
            return;
        }
    }
}

int mb_village_count_last(void) { return s_last_founded; }

/* Loyalty and rebellion, straight from WorldBox because it is the best story
 * generator in it: distance from the capital, an ambitious lord, war exhaustion
 * and misery pull a village away from its king, and when it goes it takes its
 * banner with it. */
static void loyalty_step(int v)
{
    Village *V = &mb_v[v];
    Kingdom *K = &mb_k[V->kingdom];
    if (!K->alive) return;
    Village *C = &mb_v[K->capital];
    int dist = 0;
    if (C->alive) {
        int dx = V->x - C->x, dy = V->y - C->y;
        dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
    /* Distance is the dominant term, as in WorldBox: an empire that sprawls past
     * about thirty tiles from its capital starts shedding its edges, and that is
     * the whole reason big kingdoms fracture without the player touching them. */
    int target = 40 - dist - V->exhaustion * 3 + V->happy / 4
               + (V->lord_diplo >> 2) + ((V->lord_traits & TR_LOYAL) ? 25 : 0)
               - ((V->lord_traits & TR_AMBITIOUS) ? 45 : 0);
    if (K->capital == v) target += 60;               /* the capital is loyal to itself */
    V->loyalty += (int8_t)((target > V->loyalty) ? 1 : (target < V->loyalty ? -1 : 0));

    if (V->loyalty < 0) {
        if (++V->unrest >= 8) {                      /* eight ticks under: it goes */
            int k = 0;
            for (int i = 1; i < MAXK; i++) if (!mb_k[i].alive) { k = i; break; }
            if (k) {
                memset(&mb_k[k], 0, sizeof mb_k[k]);
                mb_k[k].alive = 1; mb_k[k].sp = V->sp;
                mb_k[k].colour = (uint8_t)(k % 5);
                mb_k[k].capital = (uint8_t)v;
                mb_k[k].name = mb_name_kingdom(k * 104729u + mb_w.seed + (uint32_t)mb_w.tick);
                mb_k[k].war_with |= (uint32_t)1u << V->kingdom;
                mb_k[V->kingdom].war_with |= (uint32_t)1u << k;
                mb_chron_rebel(v, V->kingdom, k);
                V->kingdom = (uint8_t)k;
                V->loyalty = 60; V->unrest = 0;
            }
        }
    } else V->unrest = 0;
}

/* --- the kingdom brain --------------------------------------------------
 * Every 32 ticks per kingdom. Scores every neighbour it shares a border with and
 * takes the highest of war / peace / nothing. Grudges are read straight out of
 * the chronicle, which is why kings remember who sacked what. */
/* WHO FIRES, AND WHEN. A crown does not open with the bomb: it has to be at war, it has to
 * have a silo standing, and it has to be LOSING — war weariness past a threshold, or fewer
 * towns than the enemy it is fighting. That makes the strike the last act of a kingdom that
 * is going under rather than an opening move, which is both the more interesting story and
 * the reason the tree's last rung is worth reaching.
 *
 * It aims at the enemy's largest city, because that is what a desperate crown would do. */
static void nuke_think(int k)
{
    Kingdom *K = &mb_k[k];
    if (!K->alive) return;
    if (!mb_tech_known(k, TECH_NUKE)) return;
    /* NO ABSOLUTE-TIME GATE. This is the third time this exact bug has appeared, so it is
     * worth naming: king_think() runs ONE kingdom every 32 ticks, so kingdom k is visited when
     * tick % 352 == 32(k-1). Requiring `tick % 52 == 0` as well asks two unrelated schedules to
     * coincide, which for most k happens about once every eighty-eight years — and the decision
     * to launch effectively never got taken. The caravans died the same way (one delivery in
     * four hundred years) and so did the silo's build turn.
     *
     * A rate limit has to be expressed in the SCHEDULER'S OWN UNITS. The visit itself is the
     * limiter here — about once every seven years per kingdom — and the roll below is the
     * probability. */

    int silos = 0, mytowns = 0;
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive || mb_v[v].kingdom != k) continue;
        mytowns++;
        silos += mb_civ_count(v, O_SILO);
    }
    if (!silos) return;

    /* WHO FIRES FIRST. A doctrine of last resort is the better story, and it is also why no
     * strike was ever observed in nine hundred years of testing: a silo-owning kingdom had to
     * be at war AND losing at the same moment, and by the time anyone reaches the last rung of
     * a thirty-five tech tree it has usually outlived its rivals — war_with sits at zero for
     * ever. Nine silos stood in one measured world and not one was used.
     *
     * Hawkishness comes from the LORDS, which is where this game keeps character: the most
     * warlike lord in the realm, plus his ambition, his grudges and his sanity. A realm led by
     * a vengeful warlord does not wait to be attacked and does not wait to be losing — it
     * needs only a rival. A cautious one still fires solely as the last act of a kingdom going
     * under. So the bomb is offensive for SOME crowns and defensive for the rest, and which is
     * which falls out of who happens to be in charge when the silo is finished. */
    int hawk = 0;
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive || mb_v[v].kingdom != k) continue;
        int h = mb_v[v].lord_war;
        if (mb_v[v].lord_traits & TR_AMBITIOUS) h += 25;
        if (mb_v[v].lord_traits & TR_VENGEFUL)  h += 30;
        if (mb_v[v].lord_traits & TR_MADNESS)   h += 40;
        if (h > hawk) hawk = h;
    }
    /* EIGHTY, not ninety-five. Instrumented across five 900-year worlds, the two armed
     * kingdoms were: one with nine silos, a hawk of 106 and NO RIVALS LEFT — it had unified
     * the world, so there was nothing to strike — and one with a silo, two rivals, and a hawk
     * of NINETY-FOUR. One point under the bar, with no war to open the defensive path either.
     * A threshold that a realm led by a warlord misses by one point is the wrong threshold. */
    const int first_strike = hawk >= 80;

    /* The target. A hawk will hit the strongest rival in the world whether or not a war has
     * been declared; everyone else only hits an enemy it is already fighting and losing to. */
    int foe = 0, foetowns = 0;
    for (int e = 1; e < MAXK; e++) {
        if (e == k || !mb_k[e].alive) continue;
        if (!first_strike && !(K->war_with & (1u << e))) continue;
        int t = 0;
        for (int v = 1; v < MAXV; v++) if (mb_v[v].alive && mb_v[v].kingdom == e) t++;
        if (t > foetowns) { foetowns = t; foe = e; }
    }
    if (!foe) return;

    if (!first_strike) {
        if (!K->war_with) return;
        int desperate = (K->exhaustion > 35) || (foetowns >= mytowns);
        if (!desperate) return;
    }

    /* and a warlord does not deliberate for long */
    unsigned roll = mb_rand((uint32_t)(k * 40503u + (uint32_t)mb_w.tick));
    if ((roll & (first_strike ? 1u : 3u))) return;

    int target = 0;
    for (int v = 1; v < MAXV; v++)
        if (mb_v[v].alive && mb_v[v].kingdom == foe &&
            (!target || mb_v[v].pop > mb_v[target].pop)) target = v;
    if (!target) return;

    mb_nuke_strike(k, mb_v[target].x, mb_v[target].y);
    /* An unprovoked strike is a declaration in itself. */
    K->war_with |= (1u << foe);
    mb_k[foe].war_with |= (1u << k);
    K->exhaustion = (uint8_t)(K->exhaustion > 30 ? K->exhaustion - 30 : 0);
}

static void king_think(int k)
{
    nuke_think(k);
    Kingdom *K = &mb_k[k];
    if (!K->alive) return;

    /* strength and reach, gathered once */
    int my_pop = 0, my_v = 0;
    for (int v = 1; v < MAXV; v++)
        if (mb_v[v].alive && mb_v[v].kingdom == k) { my_pop += mb_v[v].pop; my_v++; }
    if (!my_v) { K->alive = 0; return; }
    K->pop = (uint16_t)my_pop;

    for (int o = 1; o < MAXK; o++) {
        if (o == k || !mb_k[o].alive) continue;
        int their_pop = mb_k[o].pop;
        int border = mb_border_len(k, o);
        if (!border) continue;

        int strength = (my_pop * 20) / (their_pop + 1);
        int grudge   = mb_chron_grudge(k, o);
        int war_score = border + strength + grudge * 6 - K->exhaustion * 3
                      + mb_age_war_bias();
        int peace_score = K->exhaustion * 4 + 30 + (mb_k[o].sp == K->sp ? 15 : 0);

        if (mb_at_war(k, o)) {
            if (peace_score > war_score) {
                K->war_with &= ~((uint32_t)1u << o);
                mb_k[o].war_with &= ~((uint32_t)1u << k);
                mb_chron_peace(k, o);
            }
        } else if (war_score > peace_score + 20 && mb_law(LAW_WAR)) {
            K->war_with |= (uint32_t)1u << o;
            mb_k[o].war_with |= (uint32_t)1u << k;
            mb_chron_war(k, o);
        }
    }

    /* war exhaustion: wars get old, which is what ends them */
    if (K->war_with) { if (K->exhaustion < 60) K->exhaustion++; }
    else if (K->exhaustion) K->exhaustion--;

    /* muster: every village of a kingdom at war raises what it can — AND every village with
     * a monster walking at it, whoever it belongs to. A kaiju used to be fought only by
     * whoever happened to be holding a spear already, so a summoned titan walked through a
     * kingdom at peace completely unopposed. A town watching Death come up the road raises
     * militia regardless of its foreign policy. */
    for (int v = 1; v < MAXV; v++) {
        if (!mb_v[v].alive || mb_v[v].kingdom != k) continue;
        int beast = 0;
        for (int ai = 0; ai < mb_agent_max() && !beast; ai++) {
            int ax, ay, ak;
            if (!mb_agent_get(ai, &ax, &ay, &ak)) continue;
            if (ak < AG_KAIJU0 || ak == AG_ANGEL) continue;   /* an angel is not a threat */
            int dx = ax - mb_v[v].x, dy = ay - mb_v[v].y;
            if (dx * dx + dy * dy <= 24 * 24) beast = 1;
        }
        mb_v[v].mustering = (uint8_t)(((K->war_with || beast) && mb_age_allows_armies())
                                      ? 1 : 0);
    }
}

/* How much border two kingdoms share, as a proxy for friction. Counting claimed
 * cells that touch a cell of the other kingdom is O(map) but it runs once per
 * kingdom per 32 ticks, and it is the only honest measure of "next to". */
int mb_border_len(int a, int b)
{
    int n = 0;
    for (int y = 1; y < MH - 1; y++)
        for (int x = 1; x < MW - 1; x++) {
            int v = mb_w.claim[AT(x, y)];
            if (!v || mb_kingdom_of(v) != a) continue;
            if (mb_kingdom_of(mb_w.claim[AT(x + 1, y)]) == b ||
                mb_kingdom_of(mb_w.claim[AT(x - 1, y)]) == b ||
                mb_kingdom_of(mb_w.claim[AT(x, y + 1)]) == b ||
                mb_kingdom_of(mb_w.claim[AT(x, y - 1)]) == b) n++;
        }
    return n;
}

/* A village whose hall is destroyed or whose people are all dead is gone; its
 * claim goes with it, so the political map shrinks back. */
static void village_death_check(int v);
static void village_death_check(int v)
{
    Village *V = &mb_v[v];
    if (V->pop > 0 && mb_is_build(mb_w.obj[AT(V->x, V->y)])) return;
    /* Twenty-six WEEKS of nobody at all. The old number was forty, but it was forty
     * VISITS on a one-village-per-tick rotation — thirty-six years — and it was set
     * that high to stop a founding party that briefly had no living member from taking
     * its kingdom down. Now that the check runs every tick, half a year of a genuinely
     * empty village is not a blip: everyone is dead. */
    if (V->grace < 26) { V->grace++; return; }
    V->grace = 0;
    mb_chron_fall(v);
    /* TAKE THE BLUEPRINT WITH IT. A pending plan writes O_PLAN onto the map, and
     * nothing ever removed it when the village that intended it died — so every
     * fallen hamlet left a blueprint square standing in the grass for ever. Making
     * village death work properly made this much more visible: a 250-year town was
     * ringed with hollow squares that no longer meant anything. */
    if (V->plan_obj && mb_in(V->plan_x, V->plan_y)
        && mb_w.obj[AT(V->plan_x, V->plan_y)] == O_PLAN)
        mb_w.obj[AT(V->plan_x, V->plan_y)] = O_NONE;
    V->plan_obj = 0;
    for (int i = 0; i < NC; i++) if (mb_w.claim[i] == v) mb_w.claim[i] = 0;
    for (int i = 0; i < mb_nu; i++) if (mb_u[i].alive && mb_u[i].village == v) mb_u[i].village = 0;
    if (mb_k[V->kingdom].capital == v) {
        /* the crown moves to the largest surviving village, or the kingdom ends */
        int best = 0, bestp = -1;
        for (int o = 1; o < MAXV; o++)
            if (o != v && mb_v[o].alive && mb_v[o].kingdom == V->kingdom && mb_v[o].pop > bestp)
                { bestp = mb_v[o].pop; best = o; }
        if (best) mb_k[V->kingdom].capital = (uint8_t)best;
        else {
            /* A DEAD KINGDOM'S WARS DIE WITH IT. Leaving the bits set left every
             * survivor permanently at war with a ghost: the 300-year curve showed
             * one war running unbroken for 240 years against a kingdom that had
             * not existed for most of them. */
            int dk = V->kingdom;
            mb_k[dk].alive = 0;
            mb_k[dk].war_with = 0;
            for (int o = 1; o < MAXK; o++) {
                mb_k[o].war_with &= ~((uint32_t)1u << dk);
                mb_k[o].ally_with &= ~((uint32_t)1u << dk);
            }
        }
    }
    V->alive = 0;
}

void mb_civ_step(void)
{
    census();

    /* EVERY village is checked for death every tick, even though only one gets the
     * full lord treatment. It used to ride along with that one-per-tick rotation, so
     * an empty village waited 47 ticks per grace point and took THIRTY-SIX YEARS to be
     * dissolved — a 400-year world carried ten ghost hamlets at any moment, holding
     * names, claims, granaries and slots in MAXV that living villages needed. The
     * check is a population test and one lookup; it is cheap enough to do properly. */
    for (int dv = 1; dv < MAXV; dv++)
        if (mb_v[dv].alive) village_death_check(dv);

    /* one village per tick gets the full treatment: its lord decides, its field
     * rebuilds if dirty, its claim creeps, its loyalty moves */
    /* VISIT A LIVING VILLAGE. This cycled all 47 slots whether or not anybody was in
     * them, so the simulation rate for civilisation scaled with how FRAGMENTED the world
     * happened to be: thirty hamlets got thirty useful ticks in forty-seven and eight
     * towns got eight. Tightening colonisation therefore slowed every town down by a
     * factor of four as a side effect — which is exactly the kind of coupling that makes
     * a balance change produce a mystery. */
    int alive_n = 0;
    for (int i = 1; i < MAXV; i++) if (mb_v[i].alive) alive_n++;
    int v = 0;
    if (alive_n) {
        int want = (int)((uint32_t)mb_w.tick % (uint32_t)alive_n), seen = 0;
        for (int i = 1; i < MAXV; i++)
            if (mb_v[i].alive && seen++ == want) { v = i; break; }
    }
    if (v && mb_v[v].alive) {
        if (mb_v[v].dirty) field_build(v);
        lord_think(v);
        succession_step(v);
        caravan_step(v);
        village_streets(v);
        village_plaza(v);
        village_gardens(v);
        try_build(v);
        claim_creep(v);
        loyalty_step(v);
        maybe_settle(v);
        village_death_check(v);
        Village *V = &mb_v[v];

        /* THE HARVEST. Farms produce; villagers do not have to walk food home for
         * the village to eat. The first version made food a gathered resource like
         * stone, and it deadlocked: with no food the lord could not afford a farm,
         * and with no farm there was no food. Worked land producing on its own is
         * both how every other sim does it and the only version that cannot lock. */
        int farms = mb_civ_count(v, O_FARM), fields = 0, n_ripe = 0, n_fallow = 0;
        for (int y = V->y - 8; y <= V->y + 8; y++)
            for (int x = V->x - 8; x <= V->x + 8; x++) {
                if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v ||
                    mb_w.biome[AT(x, y)] != B_FARM) continue;
                fields++;
                if (mb_w.obj[AT(x, y)] == O_RIPE) n_ripe++;
                if (mb_w.obj[AT(x, y)] == O_NONE) n_fallow++;
                /* AND THE CROP GROWS ON ITS OWN CLOCK, one cell at a time. A village is
                 * visited every alive_n ticks, so a one-in-eight chance per stage per visit
                 * puts a sowing about two years from the harvest — long enough to watch, and
                 * independent per cell, which is the whole point: a belt shows sown earth,
                 * green shoots, a standing crop and gold waiting to be cut, all at once. */
                uint8_t *co = &mb_w.obj[AT(x, y)];
                if ((mb_rand((uint32_t)AT(x, y) * 2246822519u) & 7u) == 0u) {
                    if      (*co == O_SOWN)   *co = O_SHOOTS;
                    else if (*co == O_SHOOTS) *co = O_GROWN;
                    else if (*co == O_GROWN)  *co = O_RIPE;
                }
            }
        /* THE BARN STORES; THE FIELD IS HARVESTED BY HAND. This was farms*6 + fields, so a
         * town with a forty-cell belt gained forty-odd food a visit whatever anybody did —
         * the granary stayed full, nothing was ever wanted, and the farmers had no reason to
         * go out. The building keeps a small yield of its own (a barn does hold something),
         * and the rest of the crop has to be carried in: mb_village_resource already treats a
         * B_FARM cell as a food target and mb_village_work already pays seven for working
         * one, so the labour existed and had simply been made pointless. */
        V->ripe   = (uint8_t)(n_ripe   > 255 ? 255 : n_ripe);
        V->fallow = (uint8_t)(n_fallow > 255 ? 255 : n_fallow);

        /* THE PASSIVE TERM COUNTS WORKED LAND ONLY. `fields / 3` paid for every cell of the
         * belt whatever state it was in, so a town gained thirteen food a visit whether or not
         * one farmer left the square. Dropping it entirely was measured too: the audit's end
         * populations fell from 222 to the eighties on half the seeds, because a hand harvest
         * alone cannot feed a town of thirty. A cell that has been SOWN pays; a fallow one does
         * not — so the yield still has to be earned, and the belt-as-work floor above keeps
         * the farmers in the fields even when the barn is full. */
        int worked = fields - n_fallow;
        V->food = (uint16_t)(V->food + farms * 5 + worked / 2);

        /* the field ring: a farm turns the ground around it into worked land,
         * which is the harvest AND the most legible sign of a working village */
        /* A BELT AS BIG AS THE MOUTHS IT FEEDS. The ceiling was a flat twenty cells for any
         * settlement, which a hamlet never reaches and a city of thirty outgrows immediately. */
        if (farms > 0 && fields < 6 + V->pop * 2) {
            int fx, fy;
            if (field_site(v, &fx, &fy)) {
                mb_w.obj[AT(fx, fy)] = O_NONE;            /* clear the scrub to sow */
                mb_w.biome[AT(fx, fy)] = B_FARM;
            }
        }

        /* AND THE VILLAGE FEEDS ITS PEOPLE. This is the point of a granary, and it
         * was missing: the store was pure bookkeeping, individual hunger could only
         * be answered by finding a bush, and starvation was by far the largest cause
         * of civ death in every run — 1973 of them in five hundred years. A villager
         * who lives beside a full barn should not starve. */
        int fed = 0;
        for (int i = 0; i < mb_nu && V->food > 0; i++) {
            Unit *u = &mb_u[i];
            if (!u->alive || u->village != v) continue;
            if (u->hunger < 40) continue;
            u->hunger = (uint8_t)(u->hunger > 70 ? u->hunger - 70 : 0);
            u->happy += 2;
            V->food--;
            fed++;
        }
        (void)fed;
        int eat = V->pop / 2;                        /* the rest is spoilage and upkeep */
        if (V->food >= eat) V->food -= (uint16_t)eat;
        else { V->food = 0; V->happy -= 6; }
        if (V->food > 600) V->food = 600;              /* a granary has a size */
    }

    /* one kingdom every 32 ticks */
    if ((mb_w.tick & 31) == 0) {
        int k = 1 + (int)(((uint32_t)mb_w.tick >> 5) % (uint32_t)(MAXK - 1));
        king_think(k);
    }
}

/* --- founding from the outside (the LIFE powers) ------------------------ */

/* Drop a founding party: a few units and, if the ground allows, a village. This
 * is the power that starts a civilisation. */
/* THE WORLD STARTS WITH PEOPLES IN IT.
 *
 * It did not, and that was the single worst thing about the game: you booted a fresh
 * world and saw deer, goats and a wolf, because founding a civilisation was a power on
 * the LIFE tab and nothing said so. "I only ever see animals — I really am not seeing
 * a game here at all" is exactly right about an empty world. WorldBox can start empty
 * because it opens on a tutorial and a toolbar the size of the screen; a handheld with
 * nine buttons has to boot into something already alive, so that the first thing you
 * do is interfere with a history rather than start one.
 *
 * Four peoples, one per race where the land allows, far enough apart to be separate
 * nations rather than one crowd — so there is politics on the first tick and a war to
 * come without you lifting a finger.
 */
void mb_civ_seed_world(int n)
{
    int got = 0;
    for (int step = 0; step < 6000 && got < n; step++) {
        uint32_t r = mb_rand((uint32_t)step * 2654435761u + 0xc1u);
        int x = (int)(r % MW), y = (int)((r >> 11) % MH);
        if (!mb_in(x, y)) continue;
        uint8_t b = mb_w.biome[AT(x, y)];
        /* somewhere a people would actually settle: not a peak, not a swamp, not ash */
        if (b != B_GRASS && b != B_MEADOW && b != B_SAVANNA && b != B_FOREST
            && b != B_BEACH && b != B_HILL) continue;
        int clear = 1;
        for (int v = 0; v < MAXV && clear; v++)
            if (mb_v[v].alive) {
                int dx = mb_v[v].x - x, dy = mb_v[v].y - y;
                if (dx * dx + dy * dy < 26 * 26) clear = 0;   /* separate nations */
            }
        if (!clear) continue;
        if (mb_civ_drop_village(SP_HUMAN + (got % SP_CIV_N), x, y) > 0) got++;
    }
    /* Relax the spacing rather than ship an empty world: a cramped archipelago is
     * still better with two neighbouring peoples on it than with none. */
    for (int step = 0; step < 6000 && got < 2; step++) {
        uint32_t r = mb_rand((uint32_t)step * 40503u + 0x51d2u);
        int x = (int)(r % MW), y = (int)((r >> 11) % MH);
        if (!mb_in(x, y) || !mb_land(mb_w.biome[AT(x, y)])) continue;
        if (mb_civ_drop_village(SP_HUMAN + (got % SP_CIV_N), x, y) > 0) got++;
    }
    (void)got;
}

int mb_civ_drop_village(int sp, int x, int y)
{
    int v = mb_village_found(sp, x, y, 0);
    if (!v) return 0;
    Village *V = &mb_v[v];
    s_last_founded = v;
    /* Ten, not six: a founding party has to survive its first winter AND find
     * two breeding pairs, and six was consistently too few. */
    for (int i = 0; i < 10; i++) {
        uint32_t r = mb_rand((uint32_t)(i * 977u + (uint32_t)mb_w.tick));
        int ux = x + (int)(r % 5) - 2, uy = y + (int)((r >> 5) % 5) - 2;
        int u = mb_unit_spawn(sp, ux, uy);
        if (u < 0) u = mb_unit_spawn(sp, x, y);
        if (u >= 0) {
            mb_u[u].village = (uint8_t)v;
            mb_u[u].family = (uint16_t)(mb_rand((uint32_t)(u * 31u)) & 63);
            /* YOUNG ADULTS. Settlers used to be 8-15 years old against a
             * twenty-year lifespan, so half the founding party died of old age
             * inside the first decade — before the lord had built a single bed —
             * and the village never reached the density where two people meet
             * often enough to have children. Every run ended the same way. */
            int span = MB_SP[SP_HUMAN + (int)(V->sp - SP_HUMAN)].lifespan;
            mb_u[u].age = (uint8_t)(span / 4 + (int)((r >> 12) & 3));
        }
    }
    /* LIVESTOCK, and only here. Sheep and hens belong beside a settlement, not
     * scattered across a wilderness — a world stocked with poultry is a farmyard.
     * A few head by the founding fire is also the fastest way to make a new village
     * read as inhabited rather than as four buildings. */
    for (int i = 0; i < 5; i++) {
        uint32_t lr = mb_rand((uint32_t)(i * 5171u + (uint32_t)mb_w.tick));
        int lx = x + (int)(lr % 7) - 3, ly = y + (int)((lr >> 6) % 7) - 3;
        mb_unit_spawn((lr & 1) ? SP_SHEEP : SP_HEN, lx, ly);
    }
    mb_v[v].food = 30; mb_v[v].wood = 12; mb_v[v].stone = 6;
    /* Clear the immediate neighbourhood of predators. A settlement founded in the
     * middle of a wolf pack is not a story, it is a formality — and driving off
     * the beasts is the first thing any real settlers would do. */
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || MB_SP[u->sp].diet != DIET_MEAT) continue;
        int dx = (u->x >> 4) - x, dy = (u->y >> 4) - y;
        if (dx * dx + dy * dy > 64) continue;
        /* pushed out to the edge of the ring, not killed: the ecology keeps them */
        int nx = x + (dx >= 0 ? 12 : -12), ny = y + (dy >= 0 ? 12 : -12);
        if (mb_unit_passable(u->sp, nx, ny)) {
            u->x = (uint16_t)(nx * 16 + 8); u->y = (uint16_t)(ny * 16 + 8);
            u->target = 0xFFFF; u->job = JOB_IDLE;
        }
    }
    return v;
}
