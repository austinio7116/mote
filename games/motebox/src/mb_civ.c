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
#include <string.h>

/* --- data --------------------------------------------------------------- */

Village mb_v[MAXV];
Kingdom mb_k[MAXK];

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
    }

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

/* --- the building ladder ------------------------------------------------
 * WorldBox's costs, near enough verbatim, because they are well tuned and they
 * make a stockpile legible: you can look at a village's store and know what it
 * is about to do. */
typedef struct { uint8_t obj, wood, stone, iron, gold, cap; const char *name; } BuildDef;
static const BuildDef BUILD[] = {
    { O_HOUSE1,   4,  0,  0,  0, 40, "house"    },
    { O_HOUSE2,   4,  4,  0,  0, 40, "house"    },
    { O_HOUSE3,  10, 10, 10, 10, 40, "house"    },
    { O_FARM,     6,  0,  0,  0,  4, "farm"     },
    { O_WOODCUT,  8,  4,  0,  0,  2, "camp"     },
    { O_MINE,     5, 10,  0,  0,  1, "mine"     },
    { O_BARRACKS, 8, 12,  4,  0,  1, "barracks" },
    { O_TEMPLE,  10, 14,  0,  4,  1, "temple"   },
    { O_TOWER,    6, 12,  0,  0,  3, "tower"    },
    { O_HALL2,   10, 10,  0,  0,  1, "hall"     },
    { O_HALL3,   10, 10, 10,  0,  1, "castle"   },
};
#define NBUILD ((int)(sizeof BUILD / sizeof BUILD[0]))

static int count_build(int v, uint8_t obj)
{
    Village *V = &mb_v[v];
    int n = 0;
    for (int y = V->y - 8; y <= V->y + 8; y++)
        for (int x = V->x - 8; x <= V->x + 8; x++)
            if (mb_in(x, y) && mb_w.claim[AT(x, y)] == v && mb_w.obj[AT(x, y)] == obj) n++;
    return n;
}

/* Somewhere to put a building: claimed, buildable, empty, and not on the hall. */
static int build_site(int v, int *ox, int *oy)
{
    Village *V = &mb_v[v];
    for (int rad = 1; rad < 8; rad++) {
        uint32_t r = mb_rand((uint32_t)(v * 131u + rad));
        int start = (int)(r & 7);
        for (int s = 0; s < 8; s++) {
            static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
            static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
            int k = (start + s) & 7;
            /* scrub is a fine site (the builders clear it); a building is not */
            int x = V->x + DX[k] * rad, y = V->y + DY[k] * rad;
            if (!mb_in(x, y) || mb_w.claim[AT(x, y)] != v) continue;
            if (!buildable(x, y)) continue;
            if (mb_is_build(mb_w.obj[AT(x, y)])) continue;
            *ox = x; *oy = y; return 1;
        }
    }
    return 0;
}

/* THE LORD'S DECISION. One per village per visit, from a needs vector weighted
 * by the lord's stewardship. The chosen building appears as a BLUEPRINT GHOST
 * immediately and only becomes real when the stockpile pays for it — so you can
 * watch a village intend something, which is the cheapest way to make the AI
 * legible (DESIGN.md 3). */
static void lord_think(int v)
{
    Village *V = &mb_v[v];
    if (V->plan_obj) return;                 /* already intending something */

    int pop = V->pop, housing = V->housing;

    /* THE PRIORITY LIST, in order, as a list rather than an if-chain — because the
     * chain had a fatal shape: whichever want came first was locked in even when
     * the village could not pay for it, and try_build then waited forever. One
     * unaffordable ambition (a tier-three hall needing iron nobody had mined) stalled
     * every village in the world at six houses and one temple. A lord picks the
     * highest thing it can AFFORD, which is also just what a competent steward does.
     */
    int want_list[12], nwant = 0;
    /* FARMS SCALE WITH MOUTHS. A flat cap of four farms fed about a dozen people, so
     * any village that grew past that starved for ever — and the audit found a world
     * that lost 2251 people to hunger while sitting at the population ceiling. Four
     * farms is a hamlet's answer; a town of forty needs seven. */
    int farm_cap = 2 + pop / 6;
    if (farm_cap > 10) farm_cap = 10;
    if (V->food < pop * 3 && count_build(v, O_FARM) < farm_cap) want_list[nwant++] = 3;
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
    if (pop >= housing && V->food > pop)                      want_list[nwant++] = 0;
    if (V->wood < 20 && count_build(v, O_WOODCUT) < 2)        want_list[nwant++] = 4;
    if (V->stone < 20 && count_build(v, O_MINE) < 1)          want_list[nwant++] = 5;
    if (V->threat > 40 && count_build(v, O_BARRACKS) < 1)     want_list[nwant++] = 6;
    if (V->hall == 1)                                         want_list[nwant++] = 9;
    if (V->hall == 2)                                         want_list[nwant++] = 10;
    if (V->hall >= 2 && count_build(v, O_TEMPLE) < 1)         want_list[nwant++] = 7;
    if (pop + 3 > housing && V->food > pop * 2)               want_list[nwant++] = 0;
    if (V->threat > 25 && count_build(v, O_TOWER) < 3)        want_list[nwant++] = 8;
    if (V->hall == 3 && housing > 0)                          want_list[nwant++] = 1;

    int want = -1;
    for (int i = 0; i < nwant; i++) {
        const BuildDef *B = &BUILD[want_list[i]];
        if (B->cap && count_build(v, B->obj) >= B->cap) continue;
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
    else if (!build_site(v, &x, &y)) return;

    V->plan_obj = B->obj; V->plan_x = (uint8_t)x; V->plan_y = (uint8_t)y;
    V->plan_i = (uint8_t)want;
    if (B->obj != O_HALL2 && B->obj != O_HALL3) mb_w.obj[AT(x, y)] = O_PLAN;
}

/* Lay an L-shaped road from a new building back to the hall. */
static void road_to_hall(int v, int bx, int by)
{
    Village *V = &mb_v[v];
    int hx = V->x, hy = V->y;
    int step = bx < hx ? 1 : -1;
    for (int x = bx; x != hx + step; x += step) {
        if (!mb_in(x, by)) break;
        int i = AT(x, by);
        if (mb_land(mb_w.biome[i]) && !mb_is_build(mb_w.obj[i])) mb_w.road[i] = 1;
    }
    step = by < hy ? 1 : -1;
    for (int y = by; y != hy + step; y += step) {
        if (!mb_in(hx, y)) break;
        int i = AT(hx, y);
        if (mb_land(mb_w.biome[i]) && !mb_is_build(mb_w.obj[i])) mb_w.road[i] = 1;
    }
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
    /* AND A ROAD TO IT. A village that builds a house and then a road to the house is
     * the difference between a scatter of huts and a town: the network is what makes
     * the buildings read as one settlement, and it grows exactly as the settlement
     * does, so its shape is a record of the order things were built in.
     *
     * An L from the new building to the hall, horizontal then vertical — the simplest
     * path that always connects, and the right-angle junctions are what the sixteen
     * road cells are for. Water is not paved: a road stops at the bank, which is also
     * why a river is a firebreak AND a bottleneck for an army. */
    road_to_hall(v, V->plan_x, V->plan_y);
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
    int need_food = V->food < V->pop * 3 ? 60 - V->food : 0;
    int need_wood = 40 - (V->wood > 40 ? 40 : V->wood);
    int need_stone = 30 - (V->stone > 30 ? 30 : V->stone);
    /* IRON AND GOLD are wanted only once the hall is ready to grow into them —
     * they were missing from this list entirely, so nobody ever mined ore and no
     * hall in any run ever reached tier three. */
    int need_iron = (V->hall >= 2 && V->iron < 14) ? 34 : 0;
    int need_gold = (V->hall >= 3 && V->gold < 14) ? 26 : 0;
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
                if (kind == CARRY_FOOD)  hit = (o == O_BUSH || o == O_FLOWER || b == B_FARM);
                if (hit) { *ox = x; *oy = y; return 1; }
            }
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
        else if (*o == O_ORE)                              { *o = O_NONE; u->carry = 6; u->carry_kind = CARRY_IRON; }
        else if (*o == O_GOLD || *o == O_SILVER || *o == O_GEM) { *o = O_NONE; u->carry = 5; u->carry_kind = CARRY_GOLD; }
        else if (*o == O_BUSH || *o == O_FLOWER)           { *o = O_NONE; u->carry = 6; u->carry_kind = CARRY_FOOD; }
        else if (b == B_MOUNTAIN || b == B_HILL)           { u->carry = 5; u->carry_kind = CARRY_STONE; }
        else if (b == B_FARM)                              { u->carry = 7; u->carry_kind = CARRY_FOOD; }
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
    if (V->pop < 18 || V->food < 30) return;
    if (mb_village_count() >= MAXV - 1) return;
    /* A COOLDOWN, not a lottery. At 1-in-64 per visit and one visit per 47 ticks,
     * the expected wait between settling attempts was three thousand ticks — sixty
     * years — so a thriving village never colonised anything and the world stayed
     * a single village however well it did. */
    if (mb_w.tick - V->last_settle < 60) return;
    V->last_settle = mb_w.tick;
    uint32_t r = mb_rand((uint32_t)(v * 6151u + 3u + (uint32_t)mb_w.tick));
    /* look for a site a comfortable distance out: far enough to be its own place,
     * near enough that the parent's people can walk there */
    for (int t = 0; t < 12; t++) {
        int ang = (int)((r >> (t + 4)) & 7);
        static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
        static const int8_t DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        int d = 10 + (int)((r >> (t + 8)) & 7);
        int x = V->x + DX[ang] * d, y = V->y + DY[ang] * d;
        if (mb_village_found(V->sp, x, y, V->kingdom)) {
            V->food -= 30;
            /* three colonists change allegiance and walk */
            int moved = 0;
            for (int i = 0; i < mb_nu && moved < 4; i++)
                if (mb_u[i].alive && mb_u[i].village == v) {
                    mb_u[i].village = (uint8_t)mb_village_count_last();
                    mb_u[i].job = JOB_IDLE; mb_u[i].target = 0xFFFF;
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
static void king_think(int k)
{
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

    /* muster: every village of a kingdom at war raises what it can */
    for (int v = 1; v < MAXV; v++)
        if (mb_v[v].alive && mb_v[v].kingdom == k)
            mb_v[v].mustering = (uint8_t)(K->war_with && mb_age_allows_armies() ? 1 : 0);
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
    int nv = MAXV - 1;
    int v = 1 + (int)((uint32_t)mb_w.tick % (uint32_t)nv);
    if (mb_v[v].alive) {
        if (mb_v[v].dirty) field_build(v);
        lord_think(v);
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
        int farms = count_build(v, O_FARM), fields = 0;
        for (int y = V->y - 8; y <= V->y + 8; y++)
            for (int x = V->x - 8; x <= V->x + 8; x++)
                if (mb_in(x, y) && mb_w.claim[AT(x, y)] == v && mb_w.biome[AT(x, y)] == B_FARM)
                    fields++;
        V->food = (uint16_t)(V->food + farms * 6 + fields);

        /* the field ring: a farm turns the ground around it into worked land,
         * which is the harvest AND the most legible sign of a working village */
        if (farms > 0) {
            int fx, fy;
            if (build_site(v, &fx, &fy) && mb_land(mb_w.biome[AT(fx, fy)])
                && mb_w.biome[AT(fx, fy)] != B_FARM && fields < 20)
                mb_w.biome[AT(fx, fy)] = B_FARM;
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
