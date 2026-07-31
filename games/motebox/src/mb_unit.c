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
#if MOTE_HOST
int mb_nodouse;
#endif
#include <string.h>

/* --- species ------------------------------------------------------------ */

/* Civ species come first so `sp < SP_DEER` is the whole "can build a village"
 * test, and the beasts follow in prey/predator order. */
/* SPRITES FROM ROGUEMOTE'S LABEL SET, not from eyeballing indices.
 *
 * roguemote/authoring/labels_{ai,human}.json labels 1306 of the master's cells with
 * a name, a category and a confidence, and labels_human wins. Two earlier passes at
 * this table guessed instead, and both were badly wrong: the first drew sheep as the
 * master's CHICKEN, wolves as its SNAKE and bears and boars both as the same COW
 * FACE; the second drew every civ race as a PORTRAIT BUST from rows 5-6 and the orc
 * as "witch/mage casting (purple)". The labels were right there the whole time.
 *
 * THE FIVE RACES ARE FIVE HUMANOID PEOPLES, all full-body figures from row 3 of the
 * characters sheet (rows 0-1 and 5-6 are busts) or the monsters sheet:
 *   human  characters[2,3]  "adult"
 *   elf    characters[7,3]  "green-hooded ranger (pointed hat)"
 *   dwarf  characters[5,3]  "dwarf"                      (human-labelled)
 *   orc    monsters  [7,4]  "goblin (green)"
 *
 * There was a fifth on monsters[2,1], whose label reads "brown troll/ogre" at LOW
 * confidence. Looked at, it is a scorpion. Four races it is — which is also exactly
 * what WorldBox ships.
 *
 * THE WILDLIFE IS WILD FIRST. The animals sheet is genuinely farm-heavy — the labels
 * count dogs, pigs, chicks, hens, lambs, sheep, ducks and geese — so the wilderness
 * is stocked from what IS wild in it (deer, the navy wolf, owl, snake, spider, bat,
 * rat, frog) plus the ox and goat off the monsters sheet, and the sheep and hens are
 * livestock that only appear near a village (see mb_unit_seed_wildlife).
 */
const MbSpecies MB_SP[SP_N] = {
    /*  name        sheet  cx cy  spd life  diet        drives      */
    { "human",     0,  2, 3, 12, 20, DIET_PLANT, DRV_CIV   },
    { "elf",       0,  7, 3, 11, 18, DIET_PLANT, DRV_CIV   },
    { "dwarf",     0,  5, 3, 10, 24, DIET_PLANT, DRV_CIV   },
    { "orc",       1,  7, 4, 13, 22, DIET_PLANT, DRV_CIV   },
    /* --- grazers and prey --- */
    { "deer",      2,  4, 0, 16, 10, DIET_PLANT, DRV_BEAST },
    { "boar",      2,  5, 0, 12, 14, DIET_PLANT, DRV_BEAST },
    { "sheep",     2, 10, 0,  9, 10, DIET_PLANT, DRV_BEAST },
    { "hen",       2,  8, 0, 10,  6, DIET_PLANT, DRV_BEAST },
    /* FIVE OF THESE DREW THE WRONG ANIMAL, which nothing caught because a cell reference is
     * just two numbers and a wrong one is still a picture. Rendering every species next to its
     * NAME found them in one look: the goat was a yellow blob off the monsters sheet, the wolf
     * was a navy creature, the rat was a wolf, the wild dog was a deer and the spider was a
     * fox. Anything that indexes art by coordinate wants a proof sheet, not a code review. */
    { "goat",      2, 11, 0, 12,  9, DIET_PLANT, DRV_BEAST },   /* brown, horned      */
    /* --- predators --- */
    { "wolf",      2,  3, 3, 17, 16, DIET_MEAT,  DRV_BEAST },   /* grey, pink eyes    */
    { "wild dog",  2,  1, 1, 15, 14, DIET_MEAT,  DRV_BEAST },   /* the orange jackal  */
    { "snake",     2,  3, 1, 11, 10, DIET_MEAT,  DRV_BEAST },
    { "spider",    1,  2, 0, 12,  8, DIET_MEAT,  DRV_BEAST },   /* red, many-legged   */
    /* --- water, swarm, vermin --- */
    { "frog",      2,  6, 4, 12,  6, DIET_PLANT, DRV_FISH  },
    { "bat",       2,  5, 1, 20,  5, DIET_MEAT,  DRV_BEAST },
    { "rat",       2,  9, 1, 14,  6, DIET_PLANT, DRV_BEAST },   /* small, pink tail   */
    /* --- what the world raises rather than breeds --- */
    /* THE RISEN WEAR BONES. "wight" pointed at characters (8,5) — a living person's cell —
     * so a graveyard stood up as a crowd of ordinary villagers, and "ghost" drew the
     * skeleton. Row 3 of the monsters sheet is seven skeletons in different poses; both
     * anchor there and mb_cast_pick spreads them across it, because a rising is a CROWD and
     * one repeated sprite is the thing that makes a crowd look like a mistake. */
    { "wight",     1,  0, 3, 12, 40, DIET_MEAT,  DRV_BEAST },
    { "ghost",     1,  7, 3, 14, 40, DIET_MEAT,  DRV_BEAST },
    { "demon",     1,  3, 7, 15, 60, DIET_MEAT,  DRV_BEAST },
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
    /* THE ECOLOGY IS A WORLD, NOT A SHARE OF THE ARRAY. The wild budget was 34% of MAXU, so
     * raising the unit cap to make room for bigger empires ALSO raised the number of wolves —
     * and measured at MAXU 768 a world opened with 285 animals against 40 founders, 37 of whom
     * were eaten inside forty years. Every civilisation was gone by year 40 and the world ran
     * three and a half centuries as a wildlife park.
     *
     * How many deer a 128x112 map supports is a fact about the map. 130 is what the tuned world
     * has always had (34% of 384), so this changes nothing today and stops the ecology inflating
     * when the ceiling for PEOPLE goes up. */
    if (sp >= SP_CIV_N) {
        int wildcap = MAXU * 34 / 100;
        if (wildcap > 80) wildcap = 80;
        /* PREDATORS AND PREY HAVE SEPARATE BUDGETS. One pool for all wildlife meant whichever
         * species bred fastest took every slot: with soldiers now clearing wolves off their own
         * fields, a measured world ended as EIGHTY DEER AND NOTHING ELSE — no wolf, no boar, no
         * goat, no bat, because deer filled the cap and nothing else could fit. A quarter of the
         * wild is meat-eaters, which is roughly what a food chain looks like, and it means a
         * hunted-out predator has a slot to come back into. */
        int pred = 0, graze = 0;
        for (int q = SP_CIV_N; q < SP_N; q++) {
            if (q >= SP_WIGHT) continue;                  /* the risen are not ecology */
            if (MB_SP[q].diet == DIET_MEAT) pred += mb_pop(q); else graze += mb_pop(q);
        }
        if (MB_SP[sp].diet == DIET_MEAT) return pred >= wildcap / 4;
        /* DOMESTIC STOCK IS NOT WILDLIFE. A town's flock carries its village id and is counted
         * separately; leaving it in this total would mean a prosperous farming world slowly
         * squeezed the deer out of existence, and every cull would open a hole in the ecology
         * that the restock would then fill with sheep. */
        graze -= mb_flock_total();
        if (graze < 0) graze = 0;
        return graze >= wildcap * 3 / 4;
    }
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

/* --- A WALL IS WORTH BUILDING -------------------------------------------
 *
 * Walls cost two wood and ten stone a cell, a town lays sixty of them, and until now they did
 * NOTHING: an enemy army walked straight through a curtain wall as if it were a hedge, so the
 * most expensive thing a lord ever built was scenery. It is also the one building whose purpose
 * a player can see at a glance, which makes it the worst possible thing to have no effect.
 *
 * The rule: a wall cell is closed to anybody who does not belong to the kingdom that raised it,
 * and to every beast. Its own people pass — they have the gate — so a walled town still works.
 * That makes an army pile up outside, which is what a siege looks like, and makes conquest
 * something that has to get THROUGH something. It cannot be absolute or a walled town would be
 * invulnerable, so a besieger breaches it: see mb_unit_siege().
 *
 * Every beast is stopped, bats included — there is no flight drive in the species table to
 * make an exception for. See the note in the body.
 */
int mb_unit_may_enter(int i, int x, int y)
{
    const Unit *u = &mb_u[i];
    if (!mb_unit_passable(u->sp, x, y)) return 0;
    int c = AT(x, y);
    if (mb_w.obj[c] != O_WALL) return 1;
    /* There is no DRV_FLY: bats are DRV_BEAST like everything else, so a wall stops them too.
     * Worth revisiting if flight ever becomes a drive of its own — a bat over a battlement is
     * correct, and it would make the air a real weakness of a walled town. */
    int owner = mb_w.claim[c];
    if (!owner || !mb_v[owner].alive) return 1;               /* a wall nobody holds is rubble */
    if (!u->village) return 0;                                 /* beasts and drifters: no */
    if (u->village == owner) return 1;
    return mb_v[u->village].kingdom == mb_v[owner].kingdom;    /* the same crown may pass */
}

/* A BESIEGER BREAKS IT DOWN. Without this a walled town could never be taken, and a rule that
 * cannot be beaten is not a rule, it is a wall around the simulation. A soldier of a kingdom at
 * war with the wall's owner, standing beside it with nothing else to do, brings it down slowly:
 * about one cell per soldier per twenty ticks, so a lone raider achieves nothing and an army
 * opens a breach in a season. The breach is real — the cell becomes rubble and stays open. */
void mb_unit_siege(int i)
{
    Unit *u = &mb_u[i];
    if (u->prof != PROF_SOLDIER || !u->village || u->sp >= SP_CIV_N) return;
    int myk = mb_v[u->village].kingdom;
    if (!myk || !mb_k[myk].war_with) return;
    int x = u->x >> 4, y = u->y >> 4;
    static const int8_t DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
    for (int k = 0; k < 4; k++) {
        int nx = x + DX[k], ny = y + DY[k];
        if (!mb_in(nx, ny)) continue;
        int c = AT(nx, ny);
        if (mb_w.obj[c] != O_WALL) continue;
        int owner = mb_w.claim[c];
        if (!owner || !mb_v[owner].alive) continue;
        if (!mb_at_war(myk, mb_v[owner].kingdom)) continue;
        if ((mb_rand((uint32_t)i * 7919u + (uint32_t)c) & 31u) != 0u) continue;
        mb_w.obj[c] = O_NONE;                    /* a breach, and it stays breached */
        mb_v[owner].dirty = 1;
#if MOTE_HOST
        {   extern uint32_t mb_breaches; mb_breaches++; }
#endif
        return;
    }
}

static int mb_unit_spawn_raw(int sp, int x, int y);

/* STOCK THE TOWN PAID FOR does not come out of the ecology's budget: a farmyard is not a
 * wilderness, and a sheep in a pen must not cost a deer its slot. The array cap still applies —
 * there is a hard limit on units however they arrive. */
int mb_unit_spawn_forced(int sp, int x, int y)
{
    if (mb_pop_all() >= MAXU - 4) return -1;
    return mb_unit_spawn_raw(sp, x, y);
}

int mb_unit_spawn(int sp, int x, int y)
{
    return mb_unit_spawn_raw(sp, x, y);
}

static int mb_unit_spawn_raw(int sp, int x, int y)
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
    /* Everybody has a given name, including the founding generation and the wildlife —
     * a named wolf costs two bytes and the chronicle can then say which wolf. */
    u->given = mb_name_person((uint32_t)slot * 40503u + r);
    if (slot >= mb_nu) mb_nu = slot + 1;
    s_pop[sp]++;
    s_births++;
    return slot;
}

static void kill(int i, int cause)
{
    Unit *u = &mb_u[i];
    if (!u->alive) return;
    /* WIDOWED. Without this a dead spouse stayed married and, worse, the slot could be
     * reused by an unrelated newborn — so somebody would find themselves wed to a child. */
    if (u->mate && u->mate - 1 < MAXU) {
        Unit *m = &mb_u[u->mate - 1];
        if (m->alive && m->mate == (uint16_t)(i + 1)) { m->mate = 0; m->happy -= 25; }
        u->mate = 0;
    }
    /* a hauler killed on the road loses what it was carrying, and the town waiting for
     * it simply never receives — which is the point of trade being a person on a road */
    if (u->job == JOB_HAUL) u->carry = 0;
    int tx = u->x >> 4, ty = u->y >> 4;
    s_pop[u->sp]--; s_deaths++;
    if (u->sp < SP_CIV_N && cause >= 0 && cause < CAUSE_N) s_dcause[cause]++;
    mb_chron_death(i, cause);
    u->alive = 0;
    /* A corpse is a grave the world remembers, and the seed the undead rising
     * needs — but only for the civ species, or the map would be all headstones.
     *
     * A GRAVE NEEDS ROOM. Without the neighbour test a settled village turned into
     * a carpet of headstones — 59 of them in one 16x14 screen, a quarter of the
     * ground, which is what "why is that town filled with ghosts?" was actually
     * looking at. Refusing to bury where two neighbours are already buried turns
     * the same deaths into a GRAVEYARD: clusters with gaps, which reads as a place
     * that buries its dead rather than a place that lost a war. */
    if (u->sp < SP_DEER && mb_in(tx, ty) && mb_w.obj[AT(tx, ty)] == O_NONE
        && mb_land(mb_w.biome[AT(tx, ty)])) {
        int near = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = tx + dx, ny = ty + dy;
                if ((dx || dy) && mb_in(nx, ny) && mb_w.obj[AT(nx, ny)] == O_GRAVE) near++;
            }
        if (near < 2) mb_w.obj[AT(tx, ty)] = O_GRAVE;
    }
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
                    } else if (o->village) {
                        /* AND A PASTURE IS NOT A HUNTING GROUND. This rule protected PEOPLE on
                         * claimed ground and said nothing about their animals, so twenty wolves
                         * ate every flock in the world as fast as it was bred: 4413 beasts raised
                         * across four hundred years with SIX standing, the town paying six food
                         * a head to feed the wolves and its own people starving for it. A wolf
                         * that walks into a village after a sheep gets speared; that it does not
                         * try is the same abstraction as it not eating the shepherd. */
                        if (mb_w.claim[AT(o->x >> 4, o->y >> 4)] == o->village) continue;
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
    /* AND THE ROAD UNDER THEIR FEET. A beaten track is a quarter faster than open ground and
     * painted tarmac is double — so a caravan really does prefer the highway, a town's streets
     * are worth paving, and the four surfaces the tech tree unlocks are four different speeds
     * rather than four textures. Only for those who walk: a fish is unimpressed. */
    if (MB_SP[u->sp].drives != DRV_FISH) {
        int g = mb_road_grade(AT(cx, cy));
        if (g >= 0) spd += spd * (5 + g * 3) / 20;        /* +25% track .. +100% tarmac */
    }
    int dx = tx - cx, dy = ty - cy;
    if (!dx && !dy) return;

    int sx = dx > 0 ? spd : (dx < 0 ? -spd : 0);
    int sy = dy > 0 ? spd : (dy < 0 ? -spd : 0);
    int nx = u->x + sx, ny = u->y + sy;
    int px = nx >> 4, py = ny >> 4;

    int self = (int)(u - mb_u);
    if (mb_unit_may_enter(self, px, py)) { u->x = (uint16_t)nx; u->y = (uint16_t)ny; return; }
    if (sx && mb_unit_may_enter(self, px, cy)) { u->x = (uint16_t)nx; return; }
    if (sy && mb_unit_may_enter(self, cx, py)) { u->y = (uint16_t)ny; return; }
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

#if MOTE_HOST
uint32_t mb_leis_seen, mb_leis_gate, mb_leis_spot, mb_leis_win;
uint32_t mb_breaches;
#endif

/* --- LEISURE: what a fed, safe, unemployed person does ----------------------
 *
 * Scored BELOW work and courtship and ABOVE the wander floor, which is precisely the gap the
 * idle population was sitting in: a town with a full granary wants no labour, so 150 of 222
 * souls had nothing scored above 14 and stood in the square.
 *
 * The pastime is mostly CHARACTER — a hash of the person, so the same villager tends to the
 * same thing and you can recognise them by it — crossed with who they are: elders and the
 * content lie in the sun, the brave go looking over the hill, the young play in the square if
 * anybody will play with them, and everybody else walks the streets.
 *
 * Its own function because TWO callers need it: the brain, and the cheap re-think an idle
 * villager gets on the ticks between its brain's turns (see mb_unit_step). The searches here
 * are a handful of random samples on purpose — the expensive part of thinking is the resource
 * spiral, and a person who has just finished a job can start walking without solving where the
 * next job is.
 *
 * Returns the job, or 0, and fills `target`.
 */
static int leisure_pick(int i, uint16_t *target)
{
    Unit *u = &mb_u[i];
    int x = u->x >> 4, y = u->y >> 4;
    if (MB_SP[u->sp].drives != DRV_CIV || !u->village) return 0;
    if (!mb_v[u->village].alive) return 0;
    const Village *V = &mb_v[u->village];
    uint32_t lh = mb_rand((uint32_t)i * 6151u + 0x1e15u);
    int pick;
    int is_lord = (u->prof == PROF_LORD);
    int old_enough = u->age > MB_SP[u->sp].lifespan * 3 / 5;
    if (old_enough || (u->happy > 40 && (lh & 3u) == 0u))       pick = JOB_BASK;
    /* a lord does not go wandering over the hill: they are expected at the hall */
    else if (!is_lord && (u->traits & TR_BRAVE) && (lh & 7u) < 3u) pick = JOB_ROAM;
    else if ((lh & 3u) == 1u)                                   pick = JOB_SPORT;
    else                                                        pick = JOB_STROLL;

    int gx = -1, gy = -1;
    if (pick == JOB_BASK) {
        /* open, soft ground out of the street: a meadow, a beach, a green */
        for (int t = 0; t < 6 && gx < 0; t++) {
            uint32_t h = mb_rand((uint32_t)(i * 977u + t * 131u));
            int ax = is_lord ? V->x : x, ay = is_lord ? V->y : y;
            int bx = ax + (int)(h % 11u) - 5, by = ay + (int)((h >> 8) % 11u) - 5;
            if (!mb_in(bx, by)) continue;
            uint8_t b = mb_w.biome[AT(bx, by)];
            if (b != B_MEADOW && b != B_GRASS && b != B_BEACH && b != B_SAVANNA) continue;
            if (mb_w.road[AT(bx, by)] || mb_w.obj[AT(bx, by)]) continue;
            gx = bx; gy = by;
        }
    } else if (pick == JOB_STROLL) {
        for (int t = 0; t < 8 && gx < 0; t++) {              /* a paved cell of their town */
            uint32_t h = mb_rand((uint32_t)(i * 1543u + t * 7717u));
            int bx = V->x + (int)(h % 17u) - 8, by = V->y + (int)((h >> 8) % 17u) - 8;
            if (!mb_in(bx, by)) continue;
            if (!mb_w.road[AT(bx, by)] || mb_w.claim[AT(bx, by)] != u->village) continue;
            gx = bx; gy = by;
        }
    } else if (pick == JOB_ROAM) {
        /* over the hill: far enough to be somewhere else, and the wander floor walks them
         * home again afterwards because it is anchored to the hall */
        for (int t = 0; t < 8 && gx < 0; t++) {
            uint32_t h = mb_rand((uint32_t)(i * 3079u + t * 40503u));
            int rad = 12 + (int)(h % 14u);
            int bx = V->x + (int)((h >> 6) % (uint32_t)(2 * rad + 1)) - rad;
            int by = V->y + (int)((h >> 17) % (uint32_t)(2 * rad + 1)) - rad;
            if (!mb_in(bx, by) || !mb_land(mb_w.biome[AT(bx, by)])) continue;
            gx = bx; gy = by;
        }
    } else {
        /* THE SQUARE, and only if somebody else of the town is near enough to play with. A
         * game of one is a person standing in a plaza, which is what this exists to stop. */
        int near_kin = 0;
        for (int q = 0; q < 6; q++) {
            int j = (i + 1 + q * 7) % (mb_nu > 0 ? mb_nu : 1);
            const Unit *o = &mb_u[j];
            if (j == i || !o->alive || o->village != u->village) continue;
            int dx = (o->x >> 4) - x, dy = (o->y >> 4) - y;
            if (dx * dx + dy * dy <= 64) { near_kin = 1; break; }
        }
        if (near_kin) {
            /* the monument or the fountain if the town has raised one; the hall otherwise */
            for (int dy = -6; dy <= 6 && gx < 0; dy++)
                for (int dx = -6; dx <= 6; dx++) {
                    int bx = V->x + dx, by = V->y + dy;
                    if (!mb_in(bx, by) || mb_w.claim[AT(bx, by)] != u->village) continue;
                    uint8_t o = mb_w.obj[AT(bx, by)];
                    if (o == O_MONUMENT || o == O_FOUNTAIN) { gx = bx; gy = by; break; }
                }
            if (gx < 0) { gx = V->x; gy = V->y; }
        }
    }
    if (gx < 0) return 0;
    *target = (uint16_t)AT(gx, gy);
    return pick;
}

/* THE CHEAP RE-THINK an idle villager gets between its brain's turns: a pastime, or the walk
 * home. Everything expensive — the resource spiral, the threat and mate queries — waits for
 * the unit's own phase. */
static void idle_pick(int i)
{
    Unit *u = &mb_u[i];
    uint16_t t = 0xFFFF;
    int job = leisure_pick(i, &t);
    if (job) { u->job = (uint8_t)job; u->target = t; return; }
    /* the wander floor, anchored to home — see the note on diffusion in think() */
    uint32_t r = mb_rand((uint32_t)i * 40503u + 3u);
    int hx = u->x >> 4, hy = u->y >> 4, span = 13;
    if (MB_SP[u->sp].drives == DRV_CIV && u->village && u->village < MAXV
        && mb_v[u->village].alive) {
        hx = mb_v[u->village].x; hy = mb_v[u->village].y;
        span = (u->prof == PROF_LORD) ? 5 : 9;      /* a lord keeps to their own square */
    }
    int wx = hx + (int)((r >> 3) % (uint32_t)span) - span / 2;
    int wy = hy + (int)((r >> 9) % (uint32_t)span) - span / 2;
    if (wx < 0) wx = 0; if (wx >= MW) wx = MW - 1;
    if (wy < 0) wy = 0; if (wy >= MH) wy = MH - 1;
    u->job = JOB_WANDER;
    u->target = (uint16_t)AT(wx, wy);
}

/* --- MEAT ---------------------------------------------------------------
 *
 * A hunter killed a deer and the carcass VANISHED: predators ate to fill their own bellies and
 * villagers only ever picked berries, so half the living things on the map were scenery to a
 * civilisation and a forest was worth less to a town than a hedge.
 *
 * Not just deer. Anything a village would butcher — the grazers and the vermin, which is every
 * plant-eating beast in the table: deer, boar, sheep, hen, goat, rat and frog. Predators are
 * not on the menu (nobody eats wolf), and killing THEM is what soldiers and JOB_FIGHT are for.
 * A carcass is worth about two and a half berry-bushes, and it has to be carried home like any
 * other load, so a hunt is a trip rather than a snack. */
static int is_game(int sp)
{
    if (sp < 0 || sp >= SP_N) return 0;
    if (sp < SP_CIV_N) return 0;                       /* people are not game */
    if (sp >= SP_WIGHT) return 0;                      /* nor is the risen dead */
    return MB_SP[sp].diet != DIET_MEAT;
}

/* The nearest animal worth killing, within a few cells. Its own scan rather than nearest()'s
 * NEAR_PREY, which is written for a predator: that one refuses to touch anything on claimed
 * ground (a rule that exists to stop wolves eating the population) and a villager hunting on
 * its own village's fields is exactly the case we want. */
static int nearest_game(int self, int x, int y)
{
    /* OUR OWN FLOCK IS A RESERVE, NOT A LARDER. Livestock stand in the pasture beside the hall,
     * so they are always the nearest thing worth killing — and with that alone deciding, a town
     * paid six food to raise a sheep, ate it within the week, and paid again: 4412 beasts raised
     * across four hundred years with FIFTEEN standing, the yield never accumulating and the food
     * going backwards. A shepherd eats the flock when there is nothing else, which is the same
     * rule the whole feature exists to express. Wild game first; the herd when it is really
     * hunger and not merely appetite. */
    const Unit *me = &mb_u[self];
    int desperate = 1;
    if (me->village && me->village < MAXV && mb_v[me->village].alive)
        desperate = mb_v[me->village].food < mb_v[me->village].pop;
    int best = -1, bestd = 1 << 30;
    for (int j = 0; j < mb_nu; j++) {
        if (j == self || !mb_u[j].alive) continue;
        if (!is_game(mb_u[j].sp)) continue;
        if (!desperate && mb_u[j].village && mb_u[j].village == me->village) continue;
        int dx = (mb_u[j].x >> 4) - x, dy = (mb_u[j].y >> 4) - y;
        int d = dx * dx + dy * dy;
        if (d > 81 || d >= bestd) continue;            /* nine cells, nearest wins */
        bestd = d; best = j;
    }
    return best;
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
        /* PANIC ONLY AT SOMETHING THAT WOULD ACTUALLY EAT YOU.
         *
         * Any meat-eater in sight was worth fifty points of danger, and JOB_FLEE outranks work
         * and courtship — but a predator's own rule (see NEAR_PREY) is that it will not touch a
         * person who is not starving, and never on claimed ground. So the two halves disagreed:
         * the wolf had no intention of attacking and the villager fled anyway.
         *
         * It did not show while predators were being hunted to extinction. Giving them their own
         * budget so they PERSIST — twenty wolves in every world, for ever — turned that
         * disagreement into an economy-wide failure: measured, 110 of 118 people fleeing at once,
         * and the audit's total end population fell from 3142 to 1895 across eight worlds. The
         * villagers were not dying, they were panicking instead of working.
         *
         * The panic now matches the rule that governs the wolf. A hungry wolf, or open ground,
         * is still worth running from. */
        if (threat >= 0) {
            int hungry = mb_u[threat].hunger >= 120;
            int home   = (sp->drives == DRV_CIV) && mb_w.claim[AT(x, y)] != 0;
            if (hungry || !home) danger += 50;
        }
    }
    /* --- FIGHT THE FIRE, if it is theirs to fight ------------------------
     *
     * This is what turns fire from an erase tool into a contest with an outcome you
     * want to watch. It follows the good implementations rather than the simulationist
     * ones: RimWorld makes firefighting a work priority that colonists can lose and
     * die doing; The Sims sends them in and lets the failure be the story; Banished
     * and Anno make it a question of whether a well is in range. Dwarf Fortress
     * famously has nobody fight it at all, which is a different and much bleaker game.
     *
     * Three rules keep it honest:
     *   - Only on CLAIMED ground. A wilderness fire burns free, which keeps the
     *     ecology and the spectacle intact; a fire in the fields is an emergency.
     *   - Only NEAR ground: a villager runs to a fire beside them, not across a
     *     continent, so a town saves itself and its neighbours do not commute.
     *   - It is DANGEROUS. Standing next to a fire hurts, and a big one hurts more
     *     than one person can fix, so towns do lose. A fight you always win is not a
     *     fight, and the losses are what make a rescued town mean anything.
     *
     * It outranks fleeing, but only just — and only while they are not standing IN
     * it. Nobody beats out a fire they are inside; that is when you run. */
    int fire_x = -1, fire_y = -1;
#if MOTE_HOST
    /* MOTEBOX_NODOUSE=1 turns firefighting off, so the feature can be measured
     * against the world where nobody lifts a bucket. A mechanic that does not change
     * the outcome is decoration, and the only way to know is to run both. */
    extern int mb_nodouse;
    if (mb_nodouse) { /* nobody fights it */ } else
#endif
    if (sp->drives == DRV_CIV && u->village && fx != FX_FIRE) {
        int bestd = 99;
        for (int dy = -5; dy <= 5; dy++)          /* the whole village turns out */
            for (int dx = -5; dx <= 5; dx++) {
                int nx = x + dx, ny = y + dy;
                if (!mb_in(nx, ny)) continue;
                int ni = AT(nx, ny);
                if (mb_fkind(mb_w.flux[ni]) != FX_FIRE) continue;
                if (mb_w.claim[ni] != u->village) continue;
                int d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; fire_x = nx; fire_y = ny; }
            }
        if (fire_x >= 0) {
            /* the brave and the loyal run toward it; a coward still runs away */
            /* Above work, forage and courtship — a fire in your own village is not
             * something you finish the harvest first for. Still below standing IN it,
             * which is handled by excluding those units above. */
            int zeal = 150;
            if (u->traits & TR_BRAVE)  zeal += 40;
            if (u->traits & TR_LOYAL)  zeal += 20;
            if (u->traits & TR_COWARD) zeal -= 120;
            if (u->hp < 40)            zeal -= 90;   /* already hurt: save yourself */
            if (zeal > bestv) { bestv = zeal; best = JOB_DOUSE;
                               u->target = (uint16_t)AT(fire_x, fire_y); }
        }
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
    /* EXCEPT THE LORD. They are a person now, and a person with an office: a lord who walks
     * off to chop wood is not running anything, and a lord found three valleys away cannot be
     * met at their own hall. They stay in town — see the leisure and wander clamps below. */
    if (u->sp < SP_DEER && u->village && u->prof != PROF_LORD) {
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
        /* --- A SOLDIER KILLS THE WOLF ----------------------------------------
         *
         * They did not. The whole fight block below is gated on already fighting or on the
         * village MUSTERING, and mustering happens for a war or for a KAIJU — which is an
         * agent, not a unit — so a wolf never triggered it. The old comment said as much:
         * "JOB_FIGHT below, which no beast ever triggers". A garrison stood in the square while
         * a bear worked its way along the hedge, and the only reason nobody died of it is that
         * a predator is separately forbidden from touching a person who is not starving or from
         * hunting on claimed ground at all. Measured across three worlds: 12 to 44 people eaten
         * in four hundred years, under 1% of deaths.
         *
         * So this is what a garrison is FOR between wars, and it is the honest half of the
         * bargain the flee rule makes: a farmhand runs from a wolf because a farmhand loses,
         * and somebody with a spear comes instead. Scored above the lord's guard and below a
         * war — a border dispute outranks vermin — and only inside the village's own reach, so
         * soldiers clear the fields rather than crusading across the map after every jackal. */
        if (u->prof == PROF_SOLDIER && u->village && mb_v[u->village].alive) {
            int th = nearest(i, x, y, NEAR_THREAT);
            if (th >= 0) {
                int bx = mb_u[th].x >> 4, by = mb_u[th].y >> 4;
                /* DEFENSIVELY, AND ONLY THAT. Anything wider is a hunt, and a hunt strips the
                 * map: with the whole CLAIM as the boundary, soldiers took every predator in the
                 * world to extinction and a measured 400-year world ended as sixty deer and one
                 * bat. Four cells is arm's reach — the wolf has come to the people, which is the
                 * case a garrison exists for. A wolf in the hills, or one merely inside the
                 * parish, is nobody's business. */
                int dxb = bx - x, dyb = by - y;
                if (dxb * dxb + dyb * dyb <= 16) {
                    int sv = 48 + (u->traits & TR_BRAVE ? 12 : 0)
                                - (u->traits & TR_COWARD ? 24 : 0);
                    if (MB_GATE(GT_CULL, sv > bestv)) {
                        bestv = sv; best = JOB_CULL;
                        u->target = (uint16_t)AT(bx, by);
                    }
                }
            }
        }

        /* war: a soldier of a kingdom at war looks for someone to fight */
        if (u->job == JOB_FIGHT || mb_village_mustering(u->village)) {
            int e = nearest(i, x, y, NEAR_ENEMY);
            int v = 55 + (u->traits & TR_BRAVE ? 20 : 0) - (u->traits & TR_COWARD ? 30 : 0);
            if (e >= 0 && v > bestv) { bestv = v; best = JOB_FIGHT; u->target = (uint16_t)AT(mb_u[e].x >> 4, mb_u[e].y >> 4); }
            /* AND A MONSTER COUNTS AS SOMEONE TO FIGHT, which it did not. The execution side
             * of JOB_FIGHT knew how to march on a kaiju and shoot it, but nothing ever CHOSE
             * the job unless an enemy UNIT was already in sight — so an army mustered against
             * a titan and then went back to hauling timber past it. Measured: six of the seven
             * beasts finished a full nine-hundred-tick life at 900/900 hit points, completely
             * unopposed. A monster in the fields is a better reason to pick up a spear than a
             * border dispute, so it scores higher. */
            if (e < 0) {
                int ax, ay, ak, bestd = 1 << 30;
                for (int ai = 0; ai < mb_agent_max(); ai++) {
                    if (!mb_agent_get(ai, &ax, &ay, &ak)) continue;
                    if (ak < AG_KAIJU0 || ak == AG_ANGEL) continue;
                    int dx = ax - x, dy = ay - y, d = dx * dx + dy * dy;
                    if (d < bestd && d <= 30 * 30) { bestd = d; }
                }
                int vb = 62 + (u->traits & TR_BRAVE ? 20 : 0)
                            - (u->traits & TR_COWARD ? 30 : 0);
                if (bestd < (1 << 30) && vb > bestv) {
                    bestv = vb; best = JOB_FIGHT; u->target = 0xFFFF;
                }
            }
        }
    }

    /* --- A VILLAGE THAT WANTS FOOD SENDS OUT HUNTERS ---------------------
     *
     * Scored against the same want the fields and the berry bushes answer, and a little above
     * it, because a carcass is worth two and a half bushes. Gated on the village actually
     * being short: a town with a full granary does not need to thin the deer, which is also
     * what keeps the ecology from being hunted flat. */
    if (sp->drives == DRV_CIV && u->village && u->prof != PROF_LORD && u->hunger < 120) {
        const Village *HV = &mb_v[u->village];
        if (HV->alive && HV->food < HV->pop * 4) {
            int g = nearest_game(i, x, y);
            if (g >= 0) {
                int hv = 26 + ((u->prof == PROF_SOLDIER || u->prof == PROF_FORESTER) ? 10 : 0)
                            + ((u->traits & TR_BRAVE) ? 6 : 0);
                if (MB_GATE(GT_HUNT, hv > bestv)) {
                    bestv = hv; best = JOB_HUNT;
                    u->target = (uint16_t)AT(mb_u[g].x >> 4, mb_u[g].y >> 4);
                }
            }
        }
    }

    /* --- THE PRIEST KEEPS THE TEMPLE --------------------------------------
     *
     * A temple fed research and the Faith ceiling; the PRIEST fed nothing, so a village with
     * twenty of them in cowls had twenty ordinary villagers hauling stone. A priest goes to the
     * temple and stays, and while they are there the people around them are consoled — which is
     * a real mechanic, because happiness feeds breeding, loyalty and therefore rebellion. It
     * also puts a small crowd at the temple door, which is what a temple is for. */
    if (u->prof == PROF_PRIEST && u->village && sp->drives == DRV_CIV) {
        const Village *TV = &mb_v[u->village];
        if (TV->alive) {
            int tx = -1, ty = -1;
            for (int dy = -8; dy <= 8 && tx < 0; dy++)
                for (int dx = -8; dx <= 8; dx++) {
                    int bx = TV->x + dx, by = TV->y + dy;
                    if (!mb_in(bx, by) || mb_w.claim[AT(bx, by)] != u->village) continue;
                    if (mb_w.obj[AT(bx, by)] == O_TEMPLE) { tx = bx; ty = by; break; }
                }
            if (tx >= 0) {
                int pv = 30 + ((u->traits & TR_LOYAL) ? 8 : 0);
                if (pv > bestv) { bestv = pv; best = JOB_PRAY; u->target = (uint16_t)AT(tx, ty); }
            }
        }
    }

    /* --- THE LORD'S GUARD ------------------------------------------------
     *
     * What an army does between wars. A soldier with nobody to fight stands near their lord,
     * which gives the square its knot of spears, gives a lord who is now a real person some
     * protection from the wolf that would otherwise eat them, and makes assassination a thing
     * that has to get past somebody. It scores below fighting and fleeing on purpose: a war or
     * a monster pulls the guard off the door, which is exactly when a town is vulnerable. */
    if (u->prof == PROF_SOLDIER && u->village && sp->drives == DRV_CIV) {
        int lu = mb_village_lord_unit(u->village);
        if (lu >= 0 && lu != i) {
            int gv = 34 + ((u->traits & TR_LOYAL) ? 12 : 0);
            if (gv > bestv) {
                bestv = gv; best = JOB_GUARD;
                u->target = (uint16_t)AT(mb_u[lu].x >> 4, mb_u[lu].y >> 4);
            }
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
    } else if (u->village && u->village < MAXV && mb_v[u->village].alive) {
        /* A FLOCK IS KEPT AT THE SIZE THE TOWN CAN FEED. Domestic stock is outside the wild
         * budget by design, and it BREEDS — so with nothing capping it a farmyard grew without
         * limit: measured, 724 sheep in one world, filling the unit array until the civ
         * population collapsed from 445 to FORTY. A farmer culls a surplus, they do not raise
         * it, so a flock at or above the town's want does not breed. This is the same number
         * village_husbandry() raises stock up to. */
        const Village *V = &mb_v[u->village];
        has_room = (int)V->flock < V->pop / 6 + 2;
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

    /* --- wander: the floor, so nothing ever stands still doing nothing ---
     *
     * A PERSON WANDERS AROUND THEIR HOME, not around wherever they happen to be.
     * This was a pure random walk from the unit's own position, and a random walk
     * DIFFUSES: over a few hundred ticks it spread every village's population evenly
     * across the map, so a village reporting eighteen people had NOBODY WITHIN FIVE
     * TILES OF ITS OWN CENTRE. The buildings were in one place and the citizens were
     * somewhere else entirely.
     *
     * That one line explains a whole run of complaints. It is why a town looked
     * deserted, why the wilderness looked full of people, and why the first version of
     * firefighting never triggered once — there was nobody near the fire to fight it.
     * Anchoring the walk to the hall makes a village a PLACE: people mill about it,
     * leave to forage or work, and come back when they have nothing to do. */
    /* --- leisure, scored: see leisure_pick() above ------------------------ */
#if MOTE_HOST
    extern uint32_t mb_leis_seen, mb_leis_gate, mb_leis_spot, mb_leis_win;
    if (sp->drives == DRV_CIV && u->village) mb_leis_seen++;
#endif
    if (sp->drives == DRV_CIV && u->village && bestv < 22
        && u->hunger < 90 && danger < 20 && u->age >= 8) {
#if MOTE_HOST
        mb_leis_gate++;
#endif
        uint16_t lt = 0xFFFF;
        int lj = leisure_pick(i, &lt);
        if (lj) {
#if MOTE_HOST
            mb_leis_spot++;
#endif
            int lv = 16 + (int)((r >> 24) & 7u) + u->happy / 12;
            if (lv > bestv) {
                bestv = lv; best = lj; u->target = lt;
#if MOTE_HOST
                mb_leis_win++;
#endif
            }
        }
    }

    if (bestv < 14) {
        best = JOB_WANDER;
        int hx = x, hy = y, span = 13;
        if (u->village && u->village < MAXV && mb_v[u->village].alive) {
            hx = mb_v[u->village].x; hy = mb_v[u->village].y;
            /* A FLOCK STAYS IN ITS PASTURE — six cells, tighter than a villager's nine, because
             * stock that wanders off is stock somebody else eats. A LORD is tightest of all,
             * being the one person who has to be findable. */
            span = (sp->drives != DRV_CIV) ? 6 : ((u->prof == PROF_LORD) ? 5 : 9);
        }
        int wx = hx + (int)((r >> 3) % span) - span / 2;
        int wy = hy + (int)((r >> 9) % span) - span / 2;
        if (wx < 0) wx = 0; if (wx >= MW) wx = MW - 1;
        if (wy < 0) wy = 0; if (wy >= MH) wy = MH - 1;
        u->target = (uint16_t)AT(wx, wy);
    }

    /* AN ERRAND OUTRANKS THE ROTA. A villager three valleys out for iron is not re-tasked
     * with the timber the village happens to want this tick — that is how the expedition
     * turned round eight ticks after setting off, every time. Danger still wins: nobody
     * carries on prospecting through a wolf. */
    if (u->mission && best != JOB_FLEE && best != JOB_FIGHT && best != JOB_DOUSE) {
        best = JOB_WORK;
        u->target = (uint16_t)(u->mission - 1);
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
        /* A VILLAGER HUNTS FOR THE TOWN, a beast hunts for itself. Same chase, different
         * ending: the beast eats where it stands, the villager carries the carcass home
         * through the ordinary JOB_WORK delivery path. */
        int civ = (sp->drives == DRV_CIV);
        int p = civ ? nearest_game(i, x, y) : nearest(i, x, y, NEAR_PREY);
        if (p < 0) { u->job = JOB_WANDER; break; }
        int px = mb_u[p].x >> 4, py = mb_u[p].y >> 4;
        if ((px - x) * (px - x) + (py - y) * (py - y) <= 2) {
            /* 35, not 60: at 60 a wolf killed a full-health villager in two bites,
             * which left no window for the villager to flee or fight back and made
             * every predator encounter a death sentence. */
            mb_u[p].hp -= (int8_t)(civ ? 45 : 35);     /* a spear beats a jaw */
            if (mb_u[p].hp <= 0) {
                kill(p, CAUSE_EATEN);
                u->kills++;
                u->happy = (int8_t)(u->happy < 118 ? u->happy + 6 : u->happy);
                if (civ) {
                    /* the carcass goes home: 15 food, against 6 for a berry bush */
                    u->carry = 15; u->carry_kind = CARRY_FOOD;
                    u->job = JOB_WORK; u->target = 0xFFFF;
                } else {
                    u->hunger = 0;
                    u->job = JOB_IDLE;
                }
            }
        } else step_toward(u, px, py);
        break;
    }

    case JOB_FIGHT: {
        int e = nearest(i, x, y, NEAR_ENEMY);
        if (e < 0) e = nearest(i, x, y, NEAR_THREAT);   /* a wild beast will do */
        /* A KAIJU IS A TARGET LIKE ANY OTHER, and it was not one. The only way anybody ever
         * hit one was to be standing next to it already with no other enemy in sight — nothing
         * walked toward it and nothing SHOT at it, so a dragon took no arrows and a titan took
         * no shells however far a kingdom had got up the weapons tree. An army marches on it
         * and fires the same rock, arrow, ball, shell or missile it would use on a rival, with
         * the same tracer, and brings it down slowly because it has nine hundred hit points. */
        if (e < 0) {
            int ax, ay, ak, bestd = 1 << 30, bx = -1, by = -1;
            for (int ai = 0; ai < mb_agent_max(); ai++) {
                if (!mb_agent_get(ai, &ax, &ay, &ak)) continue;
                if (ak < AG_KAIJU0 || ak == AG_ANGEL) continue;
                int dx = ax - x, dy = ay - y, d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; bx = ax; by = ay; }
            }
            if (bx < 0) { u->job = JOB_IDLE; break; }
            int kk2 = u->village ? mb_v[u->village].kingdom : 0;
            int shk2 = mb_war_shot_kind(kk2);
            int rng2 = mb_war_shot_range(shk2);
            /* A SPEAR DOES LESS TO A KAIJU THAN TO A MAN, which is the whole point of one.
             * At full melee damage a dozen militia deleted six thousand hit points in a few
             * ticks; this makes it a grind, and the weapons tree still matters because the
             * shot scales with it. */
            if (bestd <= 6) {                       /* in reach: strike it */
                if (mb_agent_hurt(x, y, 9 + (u->traits & TR_TOUGH ? 5 : 0))) u->kills++;
            } else if (bestd <= rng2 * rng2 && (r & 1) == 0) {
                /* from the FIGURE, in sixteenths, not from the middle of its tile: the
                 * sprites are drawn at sub-pixel positions, so tile centres put the tracer
                 * up to half a cell away from the soldier it came out of */
                mb_fx_shot((float)u->x / 16.0f, (float)u->y / 16.0f,
                           (float)bx, (float)by, shk2);
                if (mb_agent_hurt(bx, by, 3 + shk2 * 3)) u->kills++;
                int hold2 = (rng2 * 3) / 4; if (hold2 < 3) hold2 = 3;
                if (bestd > hold2 * hold2) step_toward(u, bx, by);
            } else {
                step_toward(u, bx, by);             /* march on it */
            }
            break;
        }
        int ex = mb_u[e].x >> 4, ey = mb_u[e].y >> 4;
        int d2 = (ex - x) * (ex - x) + (ey - y) * (ey - y);

        /* RANGED FIRE, which is what makes a war look like a war.
         *
         * Combat was melee only: two figures walked into each other and one lost hit points, so
         * a battle between kingdoms looked exactly like a wolf eating a deer, and five thousand
         * research points spent on gunpowder changed nothing on screen. Soldiers now shoot from
         * as far as their kingdom's weapons allow — a thrown rock at three tiles, an arrow at
         * five, a musket ball at seven, a shell at nine, a missile at twelve.
         *
         * A shot is worth less than a blow, so closing is still the way to finish somebody; and
         * the cadence is thinned, because a soldier firing every tick at ten tiles would empty a
         * battlefield before either side arrived. */
        int kk = u->village ? mb_v[u->village].kingdom : 0;
        int shk = mb_war_shot_kind(kk);
        int rng = mb_war_shot_range(shk);
        /* CAVALRY, which was a leaf: research_pick STEERS a kingdom at war toward it and
         * knowing it changed nothing whatever. A horse is reach in the other direction from a
         * bow — not distance but SPEED — so mounted troops close the ground twice as fast and
         * hit harder when they arrive. Against a kingdom with the better missile and no
         * horses, that is a real trade rather than a strictly worse branch. */
        int horse = mb_tech_known(kk, TECH_CAVALRY);

        if (d2 <= 2) {
            int dmg = 18 + (u->traits & TR_TOUGH ? 8 : 0) + (int)(r & 7) + (horse ? 7 : 0);
            mb_u[e].hp -= (int8_t)dmg;
            mb_u[e].happy -= 8;
            if (mb_u[e].hp <= 0) {
                kill(e, CAUSE_SLAIN);
                u->kills++;
                if (u->kills == 10) mb_chron_legend(i, LEGEND_KILLS);
                u->job = JOB_IDLE;
            }
        /* AND A MISSILE TROOP HOLDS ITS GROUND. This is why nobody had ever seen a rock
         * thrown. A soldier fired on one tick in four and CLOSED on the other three, and a
         * thrown rock reached three cells while melee starts at one and a half — so the window
         * in which a stone-age fighter could throw anything was one cell wide and it walked
         * through it in a single tick. Most battles produced no projectile at all.
         *
         * Now anything with reach stops at about three quarters of it and shoots from there,
         * closing only if the enemy is further off than that. A firing line forms, the tracers
         * are continuous while it holds, and the weapon tier sets how far apart the two sides
         * stand — which is what the tech tree was supposed to be buying. */
        } else if (u->sp < SP_CIV_N && d2 <= rng * rng && (r & 1) == 0) {
            mb_fx_shot((float)u->x / 16.0f, (float)u->y / 16.0f,
                       (float)mb_u[e].x / 16.0f, (float)mb_u[e].y / 16.0f, shk);
            int dmg = 7 + (int)(r >> 4 & 5) + shk * 2;      /* a better weapon hits harder */
            mb_u[e].hp -= (int8_t)dmg;
            mb_u[e].happy -= 5;
            if (mb_u[e].hp <= 0) {
                kill(e, CAUSE_SLAIN);
                u->kills++;
                if (u->kills == 10) mb_chron_legend(i, LEGEND_KILLS);
            }
            /* and they still advance while shooting, so a battle closes rather than stalling */
            int hold = (rng * 3) / 4; if (hold < 2) hold = 2;
            if (d2 > hold * hold) step_toward(u, ex, ey);   /* hold the line */
        } else {
            step_toward(u, ex, ey);
            if (horse) step_toward(u, ex, ey);        /* mounted: twice the ground a tick */
        }
        break;
    }

    /* A CARAVAN ON THE ROAD. The whole point of a hauler is that it is a PERSON walking
     * between two towns with goods on its back: it can be caught by a fire, eaten by a
     * wolf or cut down in a war, and if it is, the goods are lost and the town that was
     * waiting for them goes hungry. That is what makes a trade road worth defending. */
    case JOB_HAUL: {
        if (u->target == 0xFFFF || !u->dest) { u->job = JOB_IDLE; u->carry = 0; break; }
        int tx = u->target % MW, ty = u->target / MW;
        if ((tx - x) * (tx - x) + (ty - y) * (ty - y) <= 2) {
            mb_civ_deliver(u->dest, u->carry_kind, u->carry);
            u->carry = 0; u->dest = 0; u->job = JOB_IDLE; u->happy += 6;
        } else {
            step_toward(u, tx, ty);
            u->hunger += 1;                 /* the road is work */
        }
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
                    /* A NAME OF THEIR OWN and a trade to grow into. The surname comes
                     * from the parent, so a bloodline reads as a bloodline; the given
                     * name is theirs, so the inspect card can name somebody rather than
                     * describe a species. The trade comes from what the town needs, which
                     * is how a mining village ends up looking like one. */
                    mb_u[c].given = mb_name_person((uint32_t)c * 2654435761u
                                                   + (uint32_t)mb_w.tick);
                    mb_u[c].prof  = (uint8_t)(u->village ? mb_civ_prof_pick(u->village)
                                                        : PROF_NONE);
                    mb_chron_birth(c, i);
                }
            }
            /* MARRIED BY THE FACT OF IT. Whoever they came here to meet is their spouse
             * from now on — which is what makes a household, lets the inspect card say who
             * somebody is married to, and gives a death somebody to be widowed by. */
            if (!u->mate) {
                for (int j = 0; j < mb_nu; j++) {
                    if (j == i) continue;
                    Unit *o = &mb_u[j];
                    if (!o->alive || o->sp != u->sp || o->mate) continue;
                    int ox = o->x >> 4, oy = o->y >> 4;
                    if ((ox - x) * (ox - x) + (oy - y) * (oy - y) > 2) continue;
                    u->mate = (uint16_t)(j + 1); o->mate = (uint16_t)(i + 1);
                    break;
                }
            }
            u->hunger += 12; u->happy += 10; u->job = JOB_IDLE;
        } else step_toward(u, tx, ty);
        break;
    }

    case JOB_DOUSE: {
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        int tx = (int)(u->target % MW), ty = (int)(u->target / MW);
        if (mb_fkind(mb_w.flux[AT(tx, ty)]) != FX_FIRE) { u->job = JOB_IDLE; break; }
        int adx = tx > x ? tx - x : x - tx, ady = ty > y ? ty - y : y - ty;
        if (adx > 1 || ady > 1) { step_toward(u, tx, ty); break; }

        /* WATER IN REACH DOUBLES IT. A bucket chain needs something to fill from, so
         * a village beside a river or a lake really does defend itself better than one
         * on a dry plateau — which makes the WATER power a defensive tool and gives
         * the player a reason to dig a pond before lighting anything. */
        int wet = 0;
        for (int dy = -2; dy <= 2 && !wet; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int nx = x + dx, ny = y + dy;
                if (mb_in(nx, ny) && mb_water(mb_w.biome[AT(nx, ny)])) { wet = 1; break; }
            }
        /* Tuned against the curve, not by feel. At 2-4 a head, a dozen villagers made
         * no impression on an eleven-cell fire and it grew to 359 cells in sixty ticks;
         * the contest existed but was never winnable, so firefighting never once saved
         * anything and may as well not have been there. At these numbers a fire caught
         * in its first few ticks is CONTAINED, and one that has reached the forest is
         * not — which is the shape every game that does this well settles on, and it
         * teaches the right lesson: light the treeline, not the town square. */
        int power = wet ? 7 : 4;
        if (u->traits & TR_TOUGH) power += 2;
        int cut = mb_flux_douse(tx, ty, power);

        /* it costs them: standing beside a fire burns, and losing hurts morale */
        u->hp    -= (int8_t)(cut ? 2 : 4);
        u->happy -= (int8_t)(cut ? 1 : 3);
        if (cut) u->happy += 3;                  /* and winning is worth something */
        break;
    }

    case JOB_WORK:
        mb_village_work(u->village, i);
        break;

    /* --- LEISURE ---------------------------------------------------------
     *
     * Four pastimes, and the difference between them is meant to be visible at eight pixels:
     * a walk that follows the streets, a figure out on its own in the hills, a knot of people
     * jostling round the monument, and somebody lying still in a meadow. All four end by
     * themselves when hunger overtakes them — see the leisure block in think(). */
    case JOB_CULL: {
        /* DRIVE IT OFF. Its own job so it can never turn into a war (see JOB_CULL in mb.h), and
         * it gives up the moment the beast is out of reach: a soldier defends the people in
         * front of them, they do not pursue a wolf over the hill. */
        int th = nearest(i, x, y, NEAR_THREAT);
        if (th < 0) { u->job = JOB_IDLE; break; }
        int bx = mb_u[th].x >> 4, by = mb_u[th].y >> 4;
        int d = (bx - x) * (bx - x) + (by - y) * (by - y);
        if (d > 36) { u->job = JOB_IDLE; break; }        /* it ran; let it go */
        if (d > 2) { step_toward(u, bx, by); break; }
        mb_u[th].hp = (int8_t)(mb_u[th].hp > 45 ? mb_u[th].hp - 45 : 0);
        if (mb_u[th].hp <= 0) {
            kill(th, CAUSE_SLAIN);
            u->kills++;
            u->happy = (int8_t)(u->happy < 116 ? u->happy + 4 : u->happy);
            u->job = JOB_IDLE;
        }
        break;
    }

    case JOB_PRAY: {
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        int tx = u->target % MW, ty = u->target / MW;
        int d = (tx - x) * (tx - x) + (ty - y) * (ty - y);
        if (d > 2) { step_toward(u, tx, ty); break; }
        /* at the door: console whoever is here, and write something down */
        u->happy = (int8_t)(u->happy < 120 ? u->happy + 2 : u->happy);
        if ((mb_w.tick & 7) == 0) {
            for (int j = 0; j < mb_nu; j++) {
                if (j == i || !mb_u[j].alive || mb_u[j].village != u->village) continue;
                int dx = (mb_u[j].x >> 4) - x, dy = (mb_u[j].y >> 4) - y;
                if (dx * dx + dy * dy > 9) continue;
                if (mb_u[j].happy < 110) mb_u[j].happy += 2;
            }
            if (u->village && mb_v[u->village].alive && mb_v[u->village].research < 60000)
                mb_v[u->village].research++;
        }
        break;
    }

    case JOB_GUARD: {
        /* close, but not on top of them: a ring rather than a stack */
        int lu = mb_village_lord_unit(u->village);
        if (lu < 0) { u->job = JOB_IDLE; break; }
        int lx = mb_u[lu].x >> 4, ly = mb_u[lu].y >> 4;
        int d = (lx - x) * (lx - x) + (ly - y) * (ly - y);
        if (d > 4) step_toward(u, lx, ly);
        else if (d < 2) step_toward(u, lx + (int)(r % 3u) - 1, ly + (int)((r >> 3) % 3u) - 1);
        break;
    }

    case JOB_STROLL:
    case JOB_ROAM:
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        step_toward(u, u->target % MW, u->target / MW);
        u->happy = (int8_t)(u->happy < 120 ? u->happy + 1 : u->happy);
        if ((u->x >> 4) == (int)(u->target % MW) && (u->y >> 4) == (int)(u->target / MW))
            u->job = JOB_IDLE;                  /* arrived; the brain picks the next leg */
        break;

    case JOB_SPORT: {
        /* converge on the square, then JOSTLE: short random steps around it. A crowd doing
         * this reads as a game, where a crowd walking to one cell reads as a queue. */
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        int tx = u->target % MW, ty = u->target / MW;
        int d = (tx - x) * (tx - x) + (ty - y) * (ty - y);
        if (d > 4) step_toward(u, tx, ty);
        else {
            step_toward(u, tx + (int)(r % 3u) - 1, ty + (int)((r >> 3) % 3u) - 1);
            u->happy = (int8_t)(u->happy < 120 ? u->happy + 3 : u->happy);
        }
        break;
    }

    case JOB_BASK:
        /* walk to the spot and then STOP. Standing still is the whole point: a village where
         * three people are not moving is a village at its ease. */
        if (u->target == 0xFFFF) { u->job = JOB_IDLE; break; }
        if ((u->x >> 4) != (int)(u->target % MW) || (u->y >> 4) != (int)(u->target / MW))
            step_toward(u, u->target % MW, u->target / MW);
        else
            u->happy = (int8_t)(u->happy < 120 ? u->happy + 2 : u->happy);
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
    /* THE PREVIOUS POSITION, captured for every unit before any of them move. It has to be
     * a separate pass: brains move units in index order and a unit can be shoved by another
     * one's step, so recording it inside the same loop would give some of them this tick's
     * position as their own history and they would not move on screen at all. */
    for (int i = 0; i < mb_nu; i++) {
        if (!mb_u[i].alive) continue;
        mb_u[i].ox = mb_u[i].x; mb_u[i].oy = mb_u[i].y;
    }

    grid_build();
    int phase = (int)(mb_w.tick & 7);
    for (int i = 0; i < mb_nu; i++) {
        Unit *u = &mb_u[i];
        if (!u->alive) continue;
        /* AN IDLE PERSON THINKS AT ONCE. The stagger is what keeps two hundred brains
         * affordable, but it also meant that finishing a job — arriving with a load, or
         * finding somebody else had already taken the tree — dropped a villager into JOB_IDLE
         * for up to eight ticks with no target and no movement at all. Measured: 151 of 222
         * souls standing still at any instant, which is exactly the "swarming around doing
         * nothing" this all started from. Idle is not a state anybody should be left in, so it
         * is the one case that jumps the queue. */
        /* AND NOBODY IS LEFT STANDING STILL. The stagger is what keeps two hundred brains
         * affordable, but it also meant that finishing a job — arriving with a load, or finding
         * that somebody else had already taken the tree — dropped a villager into JOB_IDLE with
         * no target and no movement for up to eight ticks. Measured: 151 of 222 souls motionless
         * at any instant, which is exactly the "swarming about doing nothing" this started from.
         * A full re-think for all of them cost 60% more sim time at first (7.3 s to 11.7 s over
         * four hundred years, measured) — nearly all of it in mb_village_need's resource spiral,
         * which is now cached per village per tick because it never depended on the villager
         * asking. With that in place the idle can afford to think properly, which matters: the
         * cheap version (a pastime or the walk home, no work query) left a struggling village's
         * hands playing in the square for seven ticks out of eight and one audit world died out
         * quietly because of it. */
        if ((i & 7) == phase || u->job == JOB_IDLE) think(i);
        act(i);
        mb_unit_siege(i);      /* a soldier beside an enemy wall works at it */
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
     * could ever happen in it.
     *
     * AND IT IS AN ABSOLUTE NUMBER, NOT A SHARE. As `MAXU * 2 / 5` it inflated with the unit
     * cap: raising the ceiling to make room for bigger empires opened the world with 352
     * animals against 40 founders, and measured at MAXU 1024 twenty-two of the forty were
     * EATEN inside ten years and every civilisation was gone by year 40. How much game a
     * 128x112 map carries is a fact about the map.
     *
     * AND IT IS FEWER THAN IT WAS. Two fifths of 384 is 153 animals on 14336 cells — one every
     * ninety-three — which reads as a game park rather than a wilderness with things living in
     * it. Measured across four hundred years at 130, 100, 80 and 60: the population sits exactly
     * at whatever cap it is given (the ecology is cap-bound, not predation-bound) and the CIV
     * population ends at 222 in every case, so this is purely a question of how busy the world
     * looks. 94 seeded, settling to 80. */
    int cap = MAXU * 2 / 5;
    if (cap > 94) cap = 94;
    for (int tries = 0; tries < 2600 && mb_pop_all() < cap; tries++) {
        uint32_t r = mb_rand((uint32_t)tries * 2654435761u + 99u);
        int x = (int)(r % MW), y = (int)((r >> 10) % MH);
        uint8_t b = mb_w.biome[AT(x, y)];
        int sp = -1, roll = (int)((r >> 20) & 255);
        /* WILD ANIMALS IN THE WILD. Sheep and hens are livestock and are seeded
         * beside villages instead (mb_civ_drop_village), because a wilderness full
         * of poultry is a farmyard, not a world. */
        switch (b) {
        case B_FOREST:  sp = roll <  90 ? SP_DEER  : (roll < 140 ? SP_BAT   :
                             roll < 158 ? SP_WOLF  : (roll < 170 ? SP_SPIDER : -1)); break;
        case B_MEADOW:  sp = roll <  85 ? SP_DEER  : (roll < 130 ? SP_GOAT  :
                             roll < 148 ? SP_WOLF  : -1); break;
        case B_GRASS:   sp = roll <  70 ? SP_DEER  : (roll < 120 ? SP_DEER    :
                             roll < 140 ? SP_RAT   : -1); break;
        case B_SAVANNA: sp = roll <  70 ? SP_BOAR  : (roll < 115 ? SP_SNAKE :
                             roll < 132 ? SP_DOG   : -1); break;
        case B_SWAMP:   sp = roll <  70 ? SP_SNAKE : (roll < 120 ? SP_SPIDER :
                             roll < 145 ? SP_FROG  : -1); break;
        case B_DESERT:  sp = roll <  45 ? SP_SNAKE : (roll <  60 ? SP_BAT   : -1); break;
        case B_HILL:    sp = roll <  55 ? SP_GOAT  : (roll <  75 ? SP_WOLF  : -1); break;
        case B_MOUNTAIN:sp = roll <  30 ? SP_GOAT  : (roll <  42 ? SP_BAT   : -1); break;
        case B_TUNDRA:  sp = roll <  40 ? SP_DEER  : (roll <  55 ? SP_WOLF  : -1); break;
        case B_SEA: case B_SHALLOW: sp = roll < 70 ? SP_FROG : -1; break;
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
        /* MEDICINE, the third leaf: research_pick sends a kingdom with high exhaustion after
         * it and knowing it did nothing at all — sanitation had a hospital to work through,
         * medicine had no building and no effect, so a modern realm buried its people at the
         * same rate as a stone-age one. It shortens the illness and it stops the dying, which
         * are the two things a doctor does. */
        int med = 0;
        if (u->village && mb_v[u->village].alive)
            med = mb_tech_known(mb_v[u->village].kingdom, TECH_MEDICINE);

        if (!u->sick) u->sick = (uint8_t)((med ? 12 : 30)
                                          + (mb_rand((uint32_t)i * 17u) & (med ? 15 : 31)));
        if (--u->sick == 0) {
            /* SURVIVORS ARE IMMUNE, and without that a plague cannot end in a world whose
             * population sits at its ceiling: recovery alone just returns a fresh host to
             * the pool, so the audit found 1379 plague deaths still accruing at year 300.
             * Immunity is personal and lifelong, so an epidemic burns through a generation
             * and stops — and comes BACK a generation later when enough of the immune have
             * died of old age and their susceptible grandchildren fill the town. That is an
             * epidemic with a history, not a tax. */
            u->traits &= ~(TR_PLAGUE | TR_CONTAGIOUS);
            u->traits |= TR_IMMUNE;
            u->happy += 10;
            continue;
        }
        if ((mb_w.tick & 3) == 0) {
            u->hp -= (int8_t)(med ? 2 : 6); u->happy -= 4;
            if (u->hp <= 0) { kill(i, CAUSE_PLAGUE); continue; }
        }
        if (!(u->traits & TR_CONTAGIOUS)) continue;
        int x = u->x >> 4, y = u->y >> 4;
        int j = nearest(i, x, y, NEAR_MATE);
        if (j < 0) continue;
        int dx = (mb_u[j].x >> 4) - x, dy = (mb_u[j].y >> 4) - y;
        if (dx * dx + dy * dy <= 4 && !(mb_u[j].traits & (TR_PLAGUE | TR_IMMUNE))
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
            /* AND SOMETHING WORSE COMES WITH A BIG ONE. SP_DEMON existed, had a sprite, had
             * sixty years of life and the fastest legs of anything the world raises — and was
             * spawned by absolutely nothing, in any scenario, ever. A grave or two gives up a
             * skeleton; a boneyard gives up a demon, which is the escalation the rising was
             * missing and the reason a big graveyard is worth being afraid of. */
            int sp = (rr & 7) ? SP_WIGHT : SP_GHOST;
            if (n >= 8 && (rr >> 12 & 7) == 0) sp = SP_DEMON;
            int u = mb_unit_spawn(sp, x, y);
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
    /* NO BLANKET FLOOR. `wild >= 45` counted every animal together, so once the grazers filled
     * their sixty slots nothing was ever restocked — and a predator hunted off the fields by
     * soldiers could never return, however empty the meat-eaters' budget was. Measured: a world
     * of sixty deer and NOT ONE WOLF, for three centuries. mb_pop_class_full() below already
     * knows each class's budget; it is the only gate this needs. */

    /* WHICH CLASS IS SHORT. The table below is deliberately herbivore-heavy — a world that has
     * lost its wildlife needs prey back before it needs wolves, or the wolves eat the seed stock
     * — but that also meant predators got about one attempt in six while the grazers held every
     * slot, so a hunted-out wolf population never recovered: measured, fifty-nine deer and ONE
     * bat after four hundred years. When the meat-eaters' budget has room and the grazers' has
     * not, restock the thing that is missing. */
    int pred = 0, graze = 0;
    for (int q = SP_CIV_N; q < SP_N; q++) {
        if (q >= SP_WIGHT) continue;
        if (MB_SP[q].diet == DIET_MEAT) pred += mb_pop(q); else graze += mb_pop(q);
    }
    int wildcap = MAXU * 34 / 100;  if (wildcap > 80) wildcap = 80;
    int want_pred = (pred < wildcap / 4) && (graze >= wildcap * 3 / 4);

    /* AND THE RESTOCK KEEPS UP WITH THE HUNTING. Six attempts a year was written for a world
     * of 222 people who only picked berries. Four hundred and forty-five of them, with meat in
     * the diet, took one measured world's wildlife from sixty animals to EIGHT — the herd cannot
     * breed back from that. The rate now follows the shortfall, so a plundered ecology refills
     * over a few years and a full one costs almost nothing. */
    int gap = wildcap - pred - graze;  if (gap < 0) gap = 0;
    int tries = 6 + gap / 2;  if (tries > 40) tries = 40;
    for (int t = 0; t < tries; t++) {
        uint32_t r = mb_rand((uint32_t)t * 7717u + 0x1a17u);
        int x = (int)(r % MW), y = (int)((r >> 11) % MH);
        uint8_t b = mb_w.biome[AT(x, y)];
        int sp = -1, roll = (int)((r >> 22) & 255);
        if (want_pred) {
            /* the predator this ground would carry */
            switch (b) {
            case B_FOREST:  sp = (roll & 3) ? SP_WOLF : SP_SPIDER; break;
            case B_MEADOW: case B_GRASS: case B_TUNDRA: case B_HILL: sp = SP_WOLF; break;
            case B_SAVANNA: sp = (roll & 1) ? SP_DOG : SP_SNAKE; break;
            case B_SWAMP:   sp = (roll & 1) ? SP_SNAKE : SP_SPIDER; break;
            case B_DESERT:  sp = SP_SNAKE; break;
            case B_MOUNTAIN:sp = SP_BAT;   break;
            default: break;
            }
            if (sp >= 0 && !mb_pop_class_full(sp)) mb_unit_spawn(sp, x, y);
            continue;
        }
        /* herbivores overwhelmingly: a world that has lost its wildlife needs prey
         * back before it needs wolves, or the wolves simply eat the seed stock */
        switch (b) {
        case B_FOREST: case B_MEADOW:
            sp = roll < 120 ? SP_DEER : (roll < 200 ? SP_GOAT : (roll < 235 ? SP_BAT : SP_WOLF));
            break;
        case B_GRASS: case B_SAVANNA:
            sp = roll < 120 ? SP_DEER : (roll < 200 ? SP_BOAR : SP_GOAT);
            break;
        case B_SEA: case B_SHALLOW:
            sp = SP_FROG;
            break;
        default: break;
        }
        if (sp >= 0 && !mb_pop_class_full(sp)) mb_unit_spawn(sp, x, y);
    }
}
