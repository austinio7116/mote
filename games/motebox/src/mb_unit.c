/*
 * Motebox — life: units, drives, and the utility brain.
 *
 * Every living thing is one Unit: villagers, kings, deer and wolves share the
 * struct and the brain, and differ by species and by which drives they carry. A
 * wolf is a unit whose only drives are hunger, fear and breed; a villager adds
 * duty, faith and greed. That is why the ecology cost nothing extra to build.
 *
 * THE BRAIN (DESIGN.md 7, tier 1). Each tick, ONE EIGHTH of the population
 * re-scores its options and picks the best with hysteresis. Everything else just
 * continues what it was doing, which is both cheap and how animals actually
 * behave — the alternative, re-deciding every tick, produced units that stood
 * still vibrating between two equally good berries.
 *
 * All integer, all from mb_rand(seed, tick, salt): a world replays exactly.
 */
#include "mb.h"
#include <string.h>

/* --- species ------------------------------------------------------------ */

/* Civ species come first so `sp < SP_DEER` is the whole "can build a village"
 * test, and the beasts follow in prey/predator order. */
const MbSpecies MB_SP[SP_N] = {
    /*                 sheet cell  spd  hp  diet        drives      */
    { "human",     0, 3, 3, 12, 20, DIET_PLANT, DRV_CIV   },
    { "elf",       0, 5, 3, 11, 18, DIET_PLANT, DRV_CIV   },
    { "dwarf",     0, 1, 6, 10, 24, DIET_PLANT, DRV_CIV   },
    { "orc",       0, 7, 4, 13, 22, DIET_PLANT, DRV_CIV   },
    { "mushfolk",  1, 4, 6, 9,  16, DIET_PLANT, DRV_CIV   },
    { "deer",      2, 0, 0, 16, 10, DIET_PLANT, DRV_BEAST },
    { "boar",      2, 6, 2, 12, 14, DIET_PLANT, DRV_BEAST },
    { "sheep",     2, 8, 0, 9,  10, DIET_PLANT, DRV_BEAST },
    { "chicken",   2, 4, 1, 10,  6, DIET_PLANT, DRV_BEAST },
    { "rabbit",    2, 9, 3, 15,  6, DIET_PLANT, DRV_BEAST },
    { "wolf",      2, 3, 1, 17, 16, DIET_MEAT,  DRV_BEAST },
    { "bear",      2, 6, 2, 13, 26, DIET_MEAT,  DRV_BEAST },
    { "snake",     2, 9, 2, 11, 10, DIET_MEAT,  DRV_BEAST },
    { "spider",    2, 5, 3, 12,  8, DIET_MEAT,  DRV_BEAST },
    { "fish",      2, 0, 1, 12,  6, DIET_PLANT, DRV_FISH  },
    { "bee",       2, 0, 4, 20,  4, DIET_PLANT, DRV_BEAST },
    { "rat",       2, 0, 3, 14,  6, DIET_PLANT, DRV_BEAST },
    /* What the world raises rather than breeds: they eat the living, and the
     * lifespan is long because nothing about them is alive to wear out. */
    { "skeleton",  1, 0, 3, 12, 40, DIET_MEAT,  DRV_BEAST },
    { "ghost",     1, 8, 3, 14, 40, DIET_MEAT,  DRV_BEAST },
    { "demon",     1, 0, 7, 15, 60, DIET_MEAT,  DRV_BEAST },
};

/* Sheet index -> the actual sheet, resolved in mb_draw.c (which owns the images).
 * Kept as an index here so mb_unit.c never includes an asset header: the brain
 * has no business knowing what a wolf looks like. */

/* --- the array ---------------------------------------------------------- */

Unit *mb_u;
int   mb_nu;                     /* high-water mark, so scans stop early */
static int s_pop[SP_N];
static int s_births, s_deaths;
static int s_dcause[CAUSE_N];      /* why they died, for the audit and the trace */

/* ALLOC ONCE, RESET MANY. The arena is bump-only with no per-allocation free, so
 * a "new world" that called the _init functions again would leak the whole world
 * every time and run the arena dry after a handful of rerolls. Every subsystem
 * therefore splits the two. */
void mb_unit_init(void)
{
    mb_u = (Unit *)g_api->alloc(sizeof(Unit) * MAXU);
    mb_unit_reset();
}

void mb_unit_reset(void)
{
    memset(mb_u, 0, sizeof(Unit) * MAXU);
    mb_nu = 0;
    memset(s_pop, 0, sizeof s_pop);
    memset(s_dcause, 0, sizeof s_dcause);
    s_births = s_deaths = 0;
}

int mb_pop(int sp)   { return (sp >= 0 && sp < SP_N) ? s_pop[sp] : 0; }
int mb_pop_civ(void) { int n = 0; for (int s = 0; s < SP_DEER; s++) n += s_pop[s]; return n; }
int mb_pop_all(void) { int n = 0; for (int s = 0; s < SP_N; s++)    n += s_pop[s]; return n; }
int mb_pop_wild(void) { int n = 0; for (int s = SP_DEER; s < SP_N; s++) n += s_pop[s]; return n; }

/* CARRYING CAPACITY, per class rather than per world. The array is the world's
 * hard limit, and without a class share the wildlife simply filled it: 376 of 384
 * slots were deer inside eighty years, every civ birth silently failed, and a
 * village capped at thirteen people forever and could never reach the 22 it needs
 * to send settlers. An ecology having a ceiling is also just true. */
int mb_pop_class_full(int sp)
{
    if (mb_pop_all() >= MAXU - 4) return 1;
    /* The two shares must SUM to less than the array, or they are not shares: at
     * 70% civ and 45% wild the array filled, every birth of every kind failed, and
     * the wildlife then decayed to zero through predation while the civ held its
     * slots. 58 + 34 leaves 8% of slack for a spawned herd or a risen graveyard. */
    if (sp >= SP_CIV_N) return mb_pop_wild() >= MAXU * 34 / 100;
    return mb_pop_civ() >= MAXU * 58 / 100;
}
/* After a load the array is authoritative and the counters are not, so they are
 * rebuilt from it. Saving them instead lets the two disagree, and a population
 * count that lies makes every downstream decision (settling, ages, Faith) wrong. */
void mb_unit_recount(void)
{
    memset(s_pop, 0, sizeof s_pop);
    for (int i = 0; i < mb_nu; i++)
        if (mb_u[i].alive && mb_u[i].sp < SP_N) s_pop[mb_u[i].sp]++;
}

int mb_births(void)  { return s_births; }
int mb_deaths(void)  { return s_deaths; }
int mb_deaths_by(int c) { return (c >= 0 && c < CAUSE_N) ? s_dcause[c] : 0; }

/* Can this species stand here? Fish need water, everything else needs land, and
 * nothing survives standing in lava. */
int mb_unit_passable(int sp, int x, int y)
{
    if (!mb_in(x, y)) return 0;
    uint8_t b = mb_w.biome[AT(x, y)];
    if (MB_SP[sp].drives == DRV_FISH) return mb_water(b) && b != B_ICE;
    return mb_land(b);
}

int mb_unit_spawn(int sp, int x, int y)
{
    if (sp < 0 || sp >= SP_N) return -1;
    int slot = -1;
    for (int i = 0; i < MAXU; i++) if (!mb_u[i].alive) { slot = i; break; }
    if (slot < 0) return -1;                    /* the world is full */
    if (!mb_unit_passable(sp, x, y)) return -1;

    Unit *u = &mb_u[slot];
    memset(u, 0, sizeof *u);
    uint32_t r = mb_rand((uint32_t)slot * 2246822519u);
    u->alive = 1; u->sp = (uint8_t)sp;
    u->x = (uint16_t)(x * 16 + 8); u->y = (uint16_t)(y * 16 + 8);
    u->hp = 100;
    /* Age scales with the SPECIES' lifespan. A flat 4-11 was calibrated for
     * people and meant half of every chicken (lifespan 6) was spawned already
     * past dying age, so the wildlife collapsed in the first thirty years no
     * matter what it ate. */
    {
        int span = MB_SP[sp].lifespan;
        u->age = (uint8_t)(span / 6 + (int)(r & 7) * span / 24);
    }
    u->happy = 20;
    u->hunger = (uint8_t)(r >> 8 & 31);
    u->target = 0xFFFF;
    /* One in twelve is born with something: traits are rare enough to be worth
     * noticing on a soul card, common enough that a village of thirty has two. */
    if ((r >> 16 & 15) == 0) u->traits |= (uint32_t)1u << ((r >> 20) % TR_RANDOM_N);
    if (slot >= mb_nu) mb_nu = slot + 1;
    s_pop[sp]++;
    s_births++;
    return slot;
}

static void kill(int i, int cause)
{
    Unit *u = &mb_u[i];
    if (!u->alive) return;
    int tx = u->x >> 4, ty = u->y >> 4;
    s_pop[u->sp]--; s_deaths++;
    if (u->sp < SP_CIV_N && cause >= 0 && cause < CAUSE_N) s_dcause[cause]++;
    mb_chron_death(i, cause);
    u->alive = 0;
    /* A corpse is a grave the world remembers, and the seed the undead rising
     * needs — but only for the civ species, or the map would be all headstones. */
    if (u->sp < SP_DEER && mb_in(tx, ty) && mb_w.obj[AT(tx, ty)] == O_NONE
        && mb_land(mb_w.biome[AT(tx, ty)]))
        mb_w.obj[AT(tx, ty)] = O_GRAVE;
}

void mb_unit_kill(int i, int cause) { kill(i, cause); }

/* --- spatial lookup ----------------------------------------------------- *
 * A coarse bucket grid: 16x14 buckets of 8x8 tiles, each holding the index of
 * the first unit in it plus a per-unit next link. Rebuilt every tick in one pass.
 * Without it, "is there a wolf near me" was 384 units x 384 units and the brain
 * cost more than everything else in the frame put together. */
#define BW 16
#define BH 14
#define BUCK(x, y) (((y) >> 3) * BW + ((x) >> 3))
static int16_t s_head[BW * BH];
static int16_t s_link[MAXU];

static void grid_build(void)
{
    for (int i = 0; i < BW * BH; i++) s_head[i] = -1;
    for (int i = 0; i < mb_nu; i++) {
        if (!mb_u[i].alive) continue;
        int b = BUCK(mb_u[i].x >> 4, mb_u[i].y >> 4);
        if (b < 0 || b >= BW * BH) continue;
        s_link[i] = s_head[b];
        s_head[b] = (int16_t)i;
    }
}

/* Nearest unit matching `want_meat` (prey) or a same-species mate, within the
 * 3x3 buckets around (x,y). Returns -1 for none. */
static int nearest(int self, int x, int y, int mode)
{
    const Unit *me = &mb_u[self];
    int best = -1, bestd = 1 << 30;
    int bx = x >> 3, by = y >> 3;
    for (int j = by - 1; j <= by + 1; j++) {
        if (j < 0 || j >= BH) continue;
        for (int i = bx - 1; i <= bx + 1; i++) {
            if (i < 0 || i >= BW) continue;
            for (int k = s_head[j * BW + i]; k >= 0; k = s_link[k]) {
                if (k == self || !mb_u[k].alive) continue;
                const Unit *o = &mb_u[k];
                if (mode == NEAR_PREY) {
                    if (MB_SP[o->sp].diet == DIET_MEAT) continue;   /* not each other */
                    if (o->sp == me->sp) continue;
                    /* A BEAST AVOIDS PEOPLE UNLESS IT IS STARVING, and never hunts
                     * on claimed ground. Both rules exist because without them the
                     * civ population went to zero inside thirty years in every
                     * single run: villagers have to leave the claim to gather, and a
                     * world with twenty wolves in it ate them faster than they could
                     * breed. A wolf that only takes a person when it is desperate
                     * is also simply what a wolf does. */
                    if (o->sp < SP_CIV_N) {
                        if (me->hunger < 120) continue;
                        if (mb_w.claim[AT(o->x >> 4, o->y >> 4)]) continue;
                    }
                } else if (mode == NEAR_MATE) {
                    /* mature for its own species: a constant here stopped
                     * anything short-lived from ever breeding at all */
                    if (o->sp != me->sp || o->hunger > 60) continue;
                    if (o->age < MB_SP[o->sp].lifespan / 4) continue;
                } else if (mode == NEAR_THREAT) {
                    if (MB_SP[o->sp].diet != DIET_MEAT) continue;
                } else if (mode == NEAR_ENEMY) {
                    if (o->sp >= SP_DEER) continue;
                    if (mb_kingdom_of(o->village) == mb_kingdom_of(me->village)) continue;
                    if (!mb_at_war(mb_kingdom_of(me->village), mb_kingdom_of(o->village))) continue;
                }
                int dx = (o->x >> 4) - x, dy = (o->y >> 4) - y;
                int d = dx * dx + dy * dy;
                /* A hunting beast prefers WILD prey: a person costs it a big
                 * distance penalty, so it only takes one when there is nothing
                 * else about. Without this, villagers were simply the most
                 * convenient meat on the map and no civilisation survived its
                 * first thirty years. */
                if (mode == NEAR_PREY && o->sp < SP_CIV_N) d += 400;
                if (d < bestd) { bestd = d; best = k; }
            }
        }
    }
    return best;
}

int mb_unit_nearest(int self, int x, int y, int mode) { return nearest(self, x, y, mode); }

/* --- movement ----------------------------------------------------------- *
 * Greedy step toward the target with a wall slide: if the diagonal is blocked,
 * take whichever axis is open. Crowds part around obstacles instead of piling
 * into them, and it costs no pathfinding at all inside a village's own ground
 * (the flow field in Phase 4 handles the rest). */
static void step_toward(Unit *u, int tx, int ty)
{
    int spd = MB_SP[u->sp].speed;
    if (u->traits & TR_FAST) spd += spd >> 1;
    if (u->hunger > 80) spd -= spd >> 2;                 /* starving is slow */
    int cx = u->x >> 4, cy = u->y >> 4;
    int dx = tx - cx, dy = ty - cy;
    if (!dx && !dy) return;

    int sx = dx > 0 ? spd : (dx < 0 ? -spd : 0);
    int sy = dy > 0 ? spd : (dy < 0 ? -spd : 0);
    int nx = u->x + sx, ny = u->y + sy;
    int px = nx >> 4, py = ny >> 4;

    if (mb_unit_passable(u->sp, px, py)) { u->x = (uint16_t)nx; u->y = (uint16_t)ny; return; }
    if (sx && mb_unit_passable(u->sp, px, cy)) { u->x = (uint16_t)nx; return; }
    if (sy && mb_unit_passable(u->sp, cx, py)) { u->y = (uint16_t)ny; return; }
    /* boxed in: give up on this target so the brain picks something reachable */
    u->target = 0xFFFF;
}

/* --- what is edible where ----------------------------------------------- */

static int forageable(int x, int y)
{
    if (!mb_in(x, y)) return 0;
    uint8_t o = mb_w.obj[AT(x, y)], b = mb_w.biome[AT(x, y)];
    if (o == O_BUSH || o == O_FLOWER || o == O_TUFT) return 2;
    if (b == B_MEADOW || b == B_GRASS || b == B_FARM || b == B_SAVANNA) return 1;
    return 0;
}

/* Nearest forage within a radius, spiralling out. Bounded by design: a hungry
 * unit that cannot see food nearby should wander, not scan the world. */
static int find_forage(int x, int y, int r, int *ox, int *oy)
{
    for (int rad = 0; rad <= r; rad++)
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                if (rad && dx * dx + dy * dy < (rad - 1) * (rad - 1)) continue;
                int nx = x + dx, ny = y + dy;
                if (forageable(nx, ny) >= 2) { *ox = nx; *oy = ny; return 1; }
            }
    return 0;
}

/* --- the brain ---------------------------------------------------------- */

/* Actions are scored, not sequenced. The winner needs to beat the incumbent by
 * HYSTERESIS, which is what stops a unit dithering between two equal berries. */
#define HYSTERESIS 12

static void think(int i)
{
    Unit *u = &mb_u[i];
    const MbSpecies *sp = &MB_SP[u->sp];
    int x = u->x >> 4, y = u->y >> 4;
    uint32_t r = mb_rand((uint32_t)i * 40503u);

    int best = u->job, bestv = -1000;
    if (u->job != JOB_IDLE) bestv = HYSTERESIS;          /* the incumbent's edge */

    /* --- flee: threats and standing in a disaster outrank everything --- */
    int danger = 0;
    uint8_t fx = mb_fkind(mb_w.flux[AT(x, y)]);
    if (fx == FX_FIRE || fx == FX_LAVA || fx == FX_ACID) danger += 90;
    int threat = -1;
    if (sp->diet != DIET_MEAT) {
        threat = nearest(i, x, y, NEAR_THREAT);
        if (threat >= 0) danger += 50;
    }
    if (danger > bestv) { bestv = danger; best = JOB_FLEE; }

    /* A villager does NOT brawl with a wolf. It was tried: a bear does 35 a bite
     * and a farmhand does 18, so every "cornered villager fights" ended with a dead
     * villager, and wounds became the second biggest cause of civ death. Fleeing is
     * correct for a farmhand. Fighting is what soldiers, kingdoms at war and titans
     * are for — see JOB_FIGHT below, which no beast ever triggers. */

    /* --- eat --- */
    int want_food = u->hunger - 30;
    if (want_food > 0) {
        int v = want_food + ((u->traits & TR_GREEDY) ? 10 : 0);
        if (v > bestv) {
            bestv = v;
            best = (sp->diet == DIET_MEAT) ? JOB_HUNT : JOB_FORAGE;
        }
    }

    /* --- civ work: a village asks for labour, and duty answers --- */
    if (u->sp < SP_DEER && u->village) {
        int need = mb_village_need(u->village, &u->target);
        if (need > 0) {
            /* HALVED, and capped by the +15 floor rather than by the need itself.
             * At full weight the work score beat the breed score in every village
             * that wanted anything at all — which is every village — so villagers
             * worked from birth to death and not one child was ever born. A
             * civilisation whose people never breed is not a civilisation. */
            int v = 15 + need / 2 + (u->traits & TR_LOYAL ? 10 : 0) + u->happy / 6;
            if (v > 48) v = 48;
            if (v > bestv) { bestv = v; best = JOB_WORK; }
        }
        /* war: a soldier of a kingdom at war looks for someone to fight */
        if (u->job == JOB_FIGHT || mb_village_mustering(u->village)) {
            int e = nearest(i, x, y, NEAR_ENEMY);
            int v = 55 + (u->traits & TR_BRAVE ? 20 : 0) - (u->traits & TR_COWARD ? 30 : 0);
            if (e >= 0 && v > bestv) { bestv = v; best = JOB_FIGHT; u->target = (uint16_t)AT(mb_u[e].x >> 4, mb_u[e].y >> 4); }
        }
    }

    /* --- breed: only when fed, grown, and not in a panic --- */
    /* HOUSING GATES BREEDING for the civ species — WorldBox's rule, and the thing
     * that makes a house worth building. Without it one village grew to 230 people
     * on 43 beds and the lord never got past "build another house", so no hall
     * ever reached tier two and nobody ever left to settle. */
    int has_room = 1;
    if (u->sp < SP_CIV_N && u->village) {
        const Village *V = &mb_v[u->village];
        has_room = V->pop < V->housing && V->food > V->pop / 2;
    }
    if (has_room && u->hunger < 45 && u->age >= sp->lifespan / 4
        && u->age < sp->lifespan - 1 && danger < 30) {
        int mate = nearest(i, x, y, NEAR_MATE);
        if (mate >= 0) {
            /* SPARE BEDS ARE THE SIGNAL. A village with room to grow outbids its
             * own work queue; a full one does not, so labour and children trade
             * off through the housing the lord builds rather than through a
             * hand-tuned constant. */
            int spare = 0;
            if (u->sp < SP_CIV_N && u->village) {
                const Village *V = &mb_v[u->village];
                spare = (V->housing > V->pop) ? (V->housing - V->pop) : 0;
            } else spare = 4;                        /* the wild has no housing */
            int v = 40 + (spare > 6 ? 24 : spare * 4)
                  + (u->traits & TR_FERTILE ? 25 : 0) - (u->traits & TR_BARREN ? 60 : 0)
                  + u->happy / 5;
            if (v > bestv) {
                bestv = v; best = JOB_BREED;
                u->target = (uint16_t)AT(mb_u[mate].x >> 4, mb_u[mate].y >> 4);
            }
        }
    }

    /* --- wander: the floor, so nothing ever stands still doing nothing --- */
    if (bestv < 14) {
        best = JOB_WANDER;
        int wx = x + (int)((r >> 3) % 13) - 6, wy = y + (int)((r >> 9) % 13) - 6;
        if (wx < 0) wx = 0; if (wx >= MW) wx = MW - 1;
        if (wy < 0) wy = 0; if (wy >= MH) wy = MH - 1;
        u->target = (uint16_t)AT(wx, wy);
    }

    if (best != u->job) u->job = (uint8_t)best;
}

/* --- per-tick action ---------------------------------------------------- */

static void act(int i)
{
    Unit *u = &mb_u[i];
    const MbSpecies *sp = &MB_SP[u->sp];
    int x = u->x >> 4, y = u->y >> 4;
    uint32_t r = mb_rand((uint32_t)i * 2654435761u + 7u);

    switch (u->job) {

    case JOB_FLEE: {
        /* straight away from the danger, not to a destination */
        int tx = x, ty = y;
        int th = nearest(i, x, y, NEAR_THREAT);
        if (th >= 0) { tx = x - ((mb_u[th].x >> 4) - x); ty = y - ((mb_u[th].y >> 4) - y); }
        else { tx = x + (int)(r % 9) - 4; ty = y + (int)((r >> 4) % 9) - 4; }
        step_toward(u, tx, ty);
        u->happy -= 2;
        break;
    }

    case JOB_FORAGE: {
        int f = forageable(x, y);
        if (f >= 2) {                                   /* a bush: a real meal */
            mb_w.obj[AT(x, y)] = O_NONE;
            u->hunger = (uint8_t)(u->hunger < 55 ? 0 : u->hunger - 55);
            u->happy += 4;
            u->job = JOB_IDLE;
        } else if (f == 1) {                            /* grazing: slow */
            u->hunger = (uint8_t)(u->hunger < 6 ? 0 : u->hunger - 6);
        } else {
            int fx2, fy2;
            if (u->target == 0xFFFF && find_forage(x, y, 7, &fx2, &fy2))
                u->target = (uint16_t)AT(fx2, fy2);
            if (u->target != 0xFFFF) step_toward(u, u->target % MW, u->target / MW);
            else u->job = JOB_WANDER;
        }
        break;
    }

    case JOB_HUNT: {
        int p = nearest(i, x, y, NEAR_PREY);
        if (p < 0) { u->job = JOB_WANDER; break; }
        int px = mb_u[p].x >> 4, py = mb_u[p].y >> 4;
        if ((px - x) * (px - x) + (py - y) * (py - y) <= 2) {
            /* 35, not 60: at 60 a wolf killed a full-health villager in two bites,
             * which left no window for the villager to flee or fight back and made
             * every predator encounter a death sentence. */
            mb_u[p].hp -= 35;
            if (mb_u[p].hp <= 0) {
                kill(p, CAUSE_EATEN);
                u->hunger = 0; u->happy += 6;
                u->kills++;
                u->job = JOB_IDLE;
            }
        } else step_toward(u, px, py);
        break;
    }

    case JOB_FIGHT: {
        int e = nearest(i, x, y, NEAR_ENEMY);
        if (e < 0) e = nearest(i, x, y, NEAR_THREAT);   /* a beast will do */
        /* and a titan is a target too: this is how a kingdom kills a kaiju */
        if (e < 0) {
            if (mb_agent_hurt(x, y, 20 + (u->traits & TR_TOUGH ? 10 : 0))) u->kills++;
            u->job = JOB_IDLE; break;
        }
        int ex = mb_u[e].x >> 4, ey = mb_u[e].y >> 4;
        if ((ex - x) * (ex - x) + (ey - y) * (ey - y) <= 2) {
            int dmg = 18 + (u->traits & TR_TOUGH ? 8 : 0) + (int)(r & 7);
            mb_u[e].hp -= (int8_t)dmg;
            mb_u[e].happy -= 8;
            if (mb_u[e].hp <= 0) {
                kill(e, CAUSE_SLAIN);
                u->kills++;
                if (u->kills == 10) mb_chron_legend(i, LEGEND_KILLS);
                u->job = JOB_IDLE;
            }
        } else step_toward(u, ex, ey);
        break;
    }

    case JOB_BREED: {
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        int tx = u->target % MW, ty = u->target / MW;
        if ((tx - x) * (tx - x) + (ty - y) * (ty - y) <= 2) {
            if (!mb_pop_class_full(u->sp) && (r & 3) == 0) {
                int c = mb_unit_spawn(u->sp, x, y);
                if (c >= 0) {
                    mb_u[c].age = 0;
                    mb_u[c].village = u->village;
                    mb_u[c].family = u->family;
                    /* traits are inherited with a mutation, which is the whole
                     * reason bloodlines are worth watching */
                    mb_u[c].traits = u->traits;
                    if ((r >> 6 & 7) == 0)
                        mb_u[c].traits ^= (uint32_t)1u << ((r >> 9) % TR_RANDOM_N);
                    mb_chron_birth(c, i);
                }
            }
            u->hunger += 12; u->happy += 10; u->job = JOB_IDLE;
        } else step_toward(u, tx, ty);
        break;
    }

    case JOB_WORK:
        mb_village_work(u->village, i);
        break;

    case JOB_WANDER:
    default:
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        step_toward(u, u->target % MW, u->target / MW);
        if ((u->x >> 4) == (int)(u->target % MW) && (u->y >> 4) == (int)(u->target / MW))
            u->job = JOB_IDLE;
        break;
    }
}

/* --- the world's effect on a unit --------------------------------------- */

static void suffer(int i)
{
    Unit *u = &mb_u[i];
    int x = u->x >> 4, y = u->y >> 4;
    uint8_t f = mb_w.flux[AT(x, y)];
    int kind = mb_fkind(f), inten = mb_fint(f);

    if (kind == FX_FIRE)  { u->hp -= (int8_t)(6 + inten); u->happy -= 8; }
    if (kind == FX_LAVA)  { u->hp = 0; }
    if (kind == FX_ACID)  { u->hp -= (int8_t)(4 + inten); u->happy -= 6; }
    if (kind == FX_FROST) { u->hp -= (int8_t)(2 + (inten >> 1)); u->happy -= 4; }
    if (kind == FX_WATER && MB_SP[u->sp].drives != DRV_FISH) { u->hp -= 3; u->happy -= 3; }

    /* hunger, and starvation */
    int starving = 0;
    if (u->hunger < 255) u->hunger++;
    if (u->hunger > 200) { u->hp -= 2; u->happy -= 2; starving = 1; }

    /* regeneration when fed and safe */
    if (u->hunger < 60 && !kind && u->hp < 100) u->hp += (u->traits & TR_REGEN) ? 3 : 1;

    /* happiness decays toward neutral, so a mood has to be maintained */
    if (u->happy > 0) u->happy--; else if (u->happy < 0) u->happy++;

    /* Attribute the death to what actually killed it. Starvation used to be filed
     * as wounds, which sent the audit (and me) looking for a phantom combat bug. */
    if (u->hp <= 0) {
        kill(i, kind ? CAUSE_DISASTER : (starving ? CAUSE_STARVED : CAUSE_WOUNDS));
        return;
    }

    /* ageing: one year every 52 ticks, on the unit's own birthday offset so a
     * generation does not die in the same week */
    if (((mb_w.tick + i) % 52) == 0) {
        u->age++;
        int span = MB_SP[u->sp].lifespan;
        if (u->traits & TR_IMMORTAL) return;
        if (u->age > span && (int)(mb_rand((uint32_t)i * 31u) & 7) < 3) kill(i, CAUSE_AGE);
    }
}

/* --- the tick ----------------------------------------------------------- */

void mb_unit_step(void)
{
    grid_build();
    int phase = (int)(mb_w.tick & 7);
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive) continue;
        if ((i & 7) == phase) think(i);      /* one eighth of the population */
        act(i);
        suffer(i);
    }
    /* trim the high-water mark so a die-off makes the scans cheap again */
    while (mb_nu > 0 && !mb_u[mb_nu - 1].alive) mb_nu--;
}

/* --- seeding ------------------------------------------------------------ */

/* Wildlife at worldgen: densities per biome, so a forest has deer and a desert
 * does not. The predators come in at a fraction of the prey, which is enough for
 * the populations to oscillate on their own. */
void mb_unit_seed_wildlife(void)
{
    /* Leave most of the array for people. The first version spawned 383 animals
     * into a 384-slot array, so a dropped village got two settlers and every
     * later birth silently failed — the world was full of deer and nothing else
     * could ever happen in it. */
    const int cap = MAXU * 2 / 5;
    for (int tries = 0; tries < 2600 && mb_pop_all() < cap; tries++) {
        uint32_t r = mb_rand((uint32_t)tries * 2654435761u + 99u);
        int x = (int)(r % MW), y = (int)((r >> 10) % MH);
        uint8_t b = mb_w.biome[AT(x, y)];
        int sp = -1, roll = (int)((r >> 20) & 255);
        switch (b) {
        case B_FOREST:  sp = roll < 90 ? SP_DEER : (roll < 150 ? SP_RABBIT : (roll < 162 ? SP_WOLF : (roll < 168 ? SP_BEAR : -1))); break;
        case B_MEADOW:  sp = roll < 80 ? SP_DEER : (roll < 135 ? SP_SHEEP : (roll < 155 ? SP_RABBIT : (roll < 161 ? SP_WOLF : -1))); break;
        case B_GRASS:   sp = roll < 70 ? SP_SHEEP : (roll < 120 ? SP_CHICKEN : (roll < 140 ? SP_RABBIT : -1)); break;
        case B_SAVANNA: sp = roll < 60 ? SP_BOAR : (roll < 100 ? SP_SNAKE : (roll < 120 ? SP_CHICKEN : -1)); break;
        case B_SWAMP:   sp = roll < 70 ? SP_SNAKE : (roll < 120 ? SP_SPIDER : -1); break;
        case B_DESERT:  sp = roll < 40 ? SP_SNAKE : -1; break;
        case B_HILL:    sp = roll < 50 ? SP_BOAR : (roll < 62 ? SP_WOLF : -1); break;
        case B_SEA: case B_SHALLOW: sp = roll < 110 ? SP_FISH : -1; break;
        default: break;
        }
        if (sp >= 0) mb_unit_spawn(sp, x, y);
    }
}

/* --- area effects ------------------------------------------------------- *
 * Every unit-targeting power funnels through here, so the "walk the units in a
 * disc" loop exists once. Returns how many it touched, which is what a power
 * needs in order to know whether it did anything — a heal on empty ground should
 * not feel the same as a heal on a crowd. */
int mb_unit_area(int cx, int cy, int r, int op, uint32_t arg)
{
    int n = 0;
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive) continue;
        int dx = (u->x >> 4) - cx, dy = (u->y >> 4) - cy;
        if (dx * dx + dy * dy > r * r) continue;
        n++;
        switch (op) {
        case UAP_HEAL:    u->hp = 100; u->hunger = 0; u->happy += 20;
                          u->traits &= ~(TR_PLAGUE | TR_CONTAGIOUS); break;
        case UAP_HURT:    u->hp -= (int8_t)arg; if (u->hp <= 0) kill(i, CAUSE_WOUNDS); break;
        case UAP_TRAIT:   u->traits |= arg; break;
        case UAP_UNTRAIT: u->traits &= ~arg; break;
        case UAP_STARVE:  u->hunger = (uint8_t)(u->hunger + arg > 255 ? 255 : u->hunger + arg); break;
        case UAP_KILL:    kill(i, (int)arg); break;
        case UAP_HAPPY:   u->happy = (int8_t)(u->happy + 30 > 100 ? 100 : u->happy + 30); break;
        case UAP_RAGE:    u->traits |= TR_MADNESS; u->job = JOB_FIGHT; break;
        default: break;
        }
    }
    return n;
}

int mb_unit_at(int x, int y)
{
    for (int i = 0; i < mb_nu; i++)
        if (mb_u[i].alive && (mb_u[i].x >> 4) == x && (mb_u[i].y >> 4) == y) return i;
    return -1;
}

/* --- the plague --------------------------------------------------------- *
 * A SIR model with no extra state: TR_PLAGUE is infected, TR_CONTAGIOUS spreads
 * it, and healing clears both. It rides the same bucket grid the brain uses, so
 * proximity infection costs nothing extra. */
void mb_unit_plague_step(void)
{
    if (!mb_law(LAW_PLAGUE)) return;
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || !(u->traits & TR_PLAGUE)) continue;

        /* A PLAGUE HAS TO BURN OUT. The first model had infection with no
         * recovery, so it was not an epidemic, it was a slow extinction: the
         * 300-year curve showed 1618 plague deaths still climbing at eight a year
         * three centuries in, because every newborn was re-infected forever.
         * A run of illness that ends — in death OR in recovery — is what makes it
         * an event a village can survive. */
        if (!u->sick) u->sick = (uint8_t)(30 + (mb_rand((uint32_t)i * 17u) & 31));
        if (--u->sick == 0) {
            u->traits &= ~(TR_PLAGUE | TR_CONTAGIOUS);
            u->happy += 10;
            continue;
        }
        if ((mb_w.tick & 3) == 0) {
            u->hp -= 6; u->happy -= 4;
            if (u->hp <= 0) { kill(i, CAUSE_PLAGUE); continue; }
        }
        if (!(u->traits & TR_CONTAGIOUS)) continue;
        int x = u->x >> 4, y = u->y >> 4;
        int j = nearest(i, x, y, NEAR_MATE);
        if (j < 0) continue;
        int dx = (mb_u[j].x >> 4) - x, dy = (mb_u[j].y >> 4) - y;
        if (dx * dx + dy * dy <= 4 && !(mb_u[j].traits & TR_PLAGUE)
            && (mb_rand((uint32_t)i * 331u) & 7) < 3)
            mb_u[j].traits |= TR_PLAGUE | TR_CONTAGIOUS;
    }
}

/* --- the undead rising -------------------------------------------------- *
 * Graves in a radius stand up, and what they kill leaves a grave of its own for
 * the next rising. This is the one disaster whose victims become the disaster,
 * which is why it is the most frightening thing on the map even though a skeleton
 * loses to a soldier. */
int mb_unit_raise_dead(int cx, int cy, int r)
{
    int n = 0;
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if (!mb_in(x, y) || mb_w.obj[AT(x, y)] != O_GRAVE) continue;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            mb_w.obj[AT(x, y)] = O_NONE;
            uint32_t rr = mb_rand((uint32_t)AT(x, y) * 733u);
            int u = mb_unit_spawn((rr & 7) ? SP_SKELETON : SP_GHOST, x, y);
            if (u >= 0) { mb_u[u].traits |= TR_ZOMBIE; n++; }
        }
    return n;
}

/* Madness OVERRIDES the brain rather than competing with it in the utility score,
 * because that is what madness means: a mad unit attacks whatever is nearest,
 * including its own village. */
void mb_unit_madness_step(void)
{
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive || !(u->traits & TR_MADNESS)) continue;
        u->happy = -80;
        if (mb_w.tick & 1) continue;
        int x = u->x >> 4, y = u->y >> 4;
        int j = nearest(i, x, y, NEAR_MATE);
        if (j < 0) continue;
        int dx = (mb_u[j].x >> 4) - x, dy = (mb_u[j].y >> 4) - y;
        if (dx * dx + dy * dy <= 2) {
            mb_u[j].hp -= 14;
            if (mb_u[j].hp <= 0) kill(j, CAUSE_SLAIN);
        } else step_toward(u, mb_u[j].x >> 4, mb_u[j].y >> 4);
    }
}

/* --- migration ----------------------------------------------------------
 * EXTINCTION IS ABSORBING, and that is the flaw the audit found first: once the
 * wildlife hit zero — predators eating out the prey, farms replacing meadow, a
 * bad Age of Ice — there was nothing left to breed from and it stayed at zero for
 * the rest of the world's life. Five of six audited worlds ended with a dead
 * ecology.
 *
 * A trickle of migration fixes it honestly: the map is a piece of a bigger world,
 * and animals wander in from off it. Only when the population is genuinely low, so
 * a healthy ecology is never topped up and predator-prey cycles still swing.
 */
void mb_unit_migrate(void)
{
    if ((mb_w.tick % 52) != 0) return;               /* once a year */
    int wild = mb_pop_wild();
    if (wild >= 45) return;                          /* the floor, not the ceiling */

    for (int t = 0; t < 6; t++) {
        uint32_t r = mb_rand((uint32_t)t * 7717u + 0x1a17u);
        int x = (int)(r % MW), y = (int)((r >> 11) % MH);
        uint8_t b = mb_w.biome[AT(x, y)];
        int sp = -1, roll = (int)((r >> 22) & 255);
        /* herbivores overwhelmingly: a world that has lost its wildlife needs prey
         * back before it needs wolves, or the wolves simply eat the seed stock */
        switch (b) {
        case B_FOREST: case B_MEADOW:
            sp = roll < 120 ? SP_DEER : (roll < 200 ? SP_RABBIT : (roll < 230 ? SP_SHEEP : SP_WOLF));
            break;
        case B_GRASS: case B_SAVANNA:
            sp = roll < 130 ? SP_SHEEP : (roll < 210 ? SP_CHICKEN : SP_BOAR);
            break;
        case B_SEA: case B_SHALLOW:
            sp = SP_FISH;
            break;
        default: break;
        }
        if (sp >= 0 && !mb_pop_class_full(sp)) mb_unit_spawn(sp, x, y);
    }
}
