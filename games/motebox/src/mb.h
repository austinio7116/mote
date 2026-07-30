/*
 * Motebox — shared world state and the contracts between translation units.
 * See DESIGN.md. Phase 1: the world and the two views.
 */
#ifndef MB_H
#define MB_H

#include "mote_api.h"
#include "mote_build.h"

/* MOTE_GAME_MODULE() stashes the api pointer in a `static const MoteApi *mote`
 * private to game.c, so the other translation units cannot see it. They go
 * through this game-owned global instead, assigned once in g_init() — the same
 * shape roguemote uses. */
extern const MoteApi *g_api;

/* --- world geometry -----------------------------------------------------
 * 128 x 112 tiles is not arbitrary: in God's Eye one tile is ONE PIXEL, so the
 * whole world fits the screen above a 16 px HUD with no scrolling, ever. The
 * same 128x112 px region is Mortal View's camera area, which is why the zoom
 * reads as a zoom instead of a jump. */
#define MW      128
#define MH      112
#define NC      (MW * MH)
#define VIEW_H  112                  /* world rows on screen; HUD owns 112..127 */
#define TILE    8
#define MVW     (128 / TILE)         /* Mortal View: 16 tiles across */
#define MVH     (VIEW_H / TILE)      /* 14 tiles down */

/* --- biomes -------------------------------------------------------------
 * The engine autotiles straight off biome[], so these ids ARE the ruleset
 * indices: cell value t draws tiles[t-1] (mote_2d.c draw_autotile), and t > n
 * draws nothing. The order MUST match MB_TILES[] in mb_draw.c, which must match
 * the order authoring/extract_box.py prints. */
enum {
    B_NONE = 0,
    B_OCEAN, B_SEA, B_SHALLOW, B_ICE, B_BEACH, B_DESERT, B_SAVANNA, B_GRASS,
    B_SWAMP, B_HILL, B_MOUNTAIN, B_PEAK, B_TUNDRA, B_SNOW, B_ASH, B_SCORCHED,
    B_LAVA, B_ACID, B_FARM, B_RUBBLE, B_MEADOW, B_FOREST, B_ROAD,
    B_N
};
#define B_COUNT (B_N - 1)            /* number of rulesets handed to the engine */

/* Walkable for anything that walks. Water, lava and acid are not. */
static inline int mb_land(uint8_t b) {
    return b >= B_BEACH && b != B_LAVA && b != B_ACID;
}
static inline int mb_water(uint8_t b) { return b >= B_OCEAN && b <= B_ICE; }

/* --- objects: one per cell ----------------------------------------------
 * Everything from a tuft of grass to a castle occupies the same byte, because a
 * cell can only hold one thing and the alternative (a second layer) doubles the
 * map's cost for a case that never comes up. Buildings sit above O_BUILD0 so
 * "is this a building" is one comparison. */
enum {
    O_NONE = 0,
    O_TREE, O_TREE2, O_DEAD, O_BUSH, O_TUFT, O_ROCK, O_REEDS, O_FLOWER,
    O_ORE, O_SILVER, O_GOLD, O_GEM, O_BOULDER, O_PEAKROCK, O_GRAVE,
    /* --- built things (Phase 4) --- */
    O_BUILD0,
    O_FIRE_PIT = O_BUILD0,   /* the founding marker */
    O_HALL1, O_HALL2, O_HALL3,
    O_HOUSE1, O_HOUSE2, O_HOUSE3,
    O_FARM, O_MINE, O_WOODCUT, O_BARRACKS, O_TEMPLE, O_TOWER, O_DOCK, O_WALL,
    /* --- THE CIVIC LADDER, one rung per era, so a skyline dates itself ---
     * A town used to look the same in the Stone era and the Atomic one: thatch, a hall, a
     * temple and a wall, whatever its kingdom had spent twenty thousand research on. These
     * are what the tree BUILDS, and each is gated on the tech it belongs to, so you can read
     * a kingdom's era off its city without opening a menu. Two of them also fix a real hole:
     * the market and the foundry are the only sources of gold and iron this economy has. */
    O_GRANARY,               /* pottery      — a bigger store against famine */
    O_MARKET,                /* currency     — GOLD income                  */
    O_LIBRARY,               /* writing      — research                     */
    O_FOUNDRY,               /* metallurgy   — IRON income                  */
    O_COLLEGE,               /* university   — research, a lot of it        */
    O_HOSPITAL,              /* sanitation   — the plague burns out faster  */
    O_FACTORY,               /* steam        — timber and stone production  */
    O_STATION,               /* the railway  — caravans go further, oftener */
    O_POWER,                 /* electricity  — research, and it looks modern */
    O_SILO,                  /* the bomb: the end of the tree, made concrete */
    /* --- THE PLAZA, which is not a rung on any ladder -----------------------
     * These are the only built things with no yield at all. A town that has grown past a
     * hamlet paves a square by its hall and puts something in it, because a settlement of
     * pure function looks like a depot — see village_plaza(). */
    O_MONUMENT,              /* TOWN tier — a column with the crown's banner on it */
    O_FOUNTAIN,              /* CITY tier — the only water a city makes itself     */
    O_PLAN,                  /* a blueprint ghost: decided, not yet paid for */
    O_N
};
static inline int mb_is_build(uint8_t o) { return o >= O_BUILD0 && o < O_N; }

/* --- flux: the disaster channel ----------------------------------------
 * One byte per cell, kind:4 / intensity:4. Every disaster in DESIGN.md 10 is a
 * case in mb_flux_step()'s switch, so they share spread, decay, wind and the
 * terrain consequences instead of each owning a system. */
enum { FX_NONE = 0, FX_FIRE, FX_LAVA, FX_WATER, FX_ACID, FX_FROST, FX_N };
/* disasters that WALK rather than spread: a place that keeps acting for a while */
enum { AG_TORNADO = 0, AG_VENT,
       AG_KAIJU0, AG_TITAN = AG_KAIJU0, AG_MEDUSA, AG_REAPER, AG_DRAGON,
       AG_GOLEM, AG_EYE, AG_ANGEL,
       AG_MAW,                       /* the endgame void: it never stops */
       AG_N };
static inline uint8_t mb_fkind(uint8_t f) { return (uint8_t)(f >> 4); }
static inline uint8_t mb_fint(uint8_t f)  { return (uint8_t)(f & 15); }

/* FX sheets: the six elemental recolours of the master's mono FX band. */
enum { FXE_FIRE = 0, FXE_FROST, FXE_ACID, FXE_ASH, FXE_HOLY, FXE_VOID, FXE_N };
/* particle shapes, each a cell run in those sheets */
enum { PK_SPARK = 0, PK_SMOKE, PK_RING, PK_BOLT, PK_GUST, PK_STAR, PK_N };

/* --- life ---------------------------------------------------------------
 * One struct for villagers, kings, deer and wolves: they share the brain and
 * differ by species and by which drives they carry. That is why the ecology cost
 * nothing extra to build. */
#define MAXU 384

enum {
    /* FOUR HUMANOID PEOPLES, and they must stay first: `sp < SP_CIV_N` is the whole
     * "can found a village" test. Four is also exactly what WorldBox has.
     * There was a fifth, monsters[2,1], on a label of "brown troll/ogre" with LOW
     * confidence. It is a scorpion. A low-confidence label is a hint, not an answer. */
    SP_HUMAN = 0, SP_ELF, SP_DWARF, SP_ORC,
    SP_DEER, SP_BOAR, SP_SHEEP, SP_HEN, SP_GOAT,           /* grazers and prey */
    SP_WOLF, SP_DOG, SP_SNAKE, SP_SPIDER,                  /* predators */
    SP_FROG, SP_BAT, SP_RAT,                               /* water, swarm, vermin */
    SP_WIGHT, SP_GHOST, SP_DEMON,                          /* what the world raises */
    SP_N
};
#define SP_CIV_N SP_DEER          /* four races: human, elf, dwarf, orc */                     /* species below this build things */

enum { DIET_PLANT = 0, DIET_MEAT };
enum { DRV_CIV = 0, DRV_BEAST, DRV_FISH };   /* which drive set a species carries */
enum { JOB_IDLE = 0, JOB_WANDER, JOB_FORAGE, JOB_HUNT, JOB_FLEE, JOB_BREED,
       JOB_WORK, JOB_FIGHT, JOB_DOUSE, JOB_HAUL, JOB_N };

/* PROFESSIONS. A job is what somebody is doing this minute; a profession is what they
 * are for. Splitting them is what lets a village have a shape — nine farmers and one
 * smith is a farming village, and you can read that off the crowd without a legend,
 * because a profession also chooses which figure is drawn. */
enum { PROF_NONE = 0, PROF_FARMER, PROF_FORESTER, PROF_MINER, PROF_SOLDIER,
       PROF_PRIEST, PROF_TRADER, PROF_BUILDER, PROF_LORD, PROF_N };
extern const char *const MB_PROF_NAME[PROF_N];

/* CREEDS. A village believes something, it spreads to its neighbours along roads and
 * trade, and a kingdom whose towns share a creed holds together better than one whose
 * towns do not. BLESS and CURSE convert, which is how a god plays this. */
enum { CREED_NONE = 0, CREED_SUN, CREED_MOON, CREED_STONE, CREED_TIDE, CREED_N };
extern const char *const MB_CREED_NAME[CREED_N];

/* SETTLEMENT TIERS, from population and what is standing. A tier is not decoration: it
 * gates what the lord will attempt and how far the street plan reaches. */
enum { TIER_CAMP = 0, TIER_HAMLET, TIER_VILLAGE, TIER_TOWN, TIER_CITY, TIER_N };
extern const char *const MB_TIER_NAME[TIER_N];

/* --- THE TECH TREE ------------------------------------------------------
 *
 * What this replaces was a LINE of seven: tools, farming, masonry, weapons, writing,
 * seafaring, engineering, held as a COUNT, so "do you know masonry" was `tech >= 3`. It had
 * two problems and the second is worse than the first. There were no branches, so every
 * kingdom in every world followed the identical path; and every kingdom REACHED THE END —
 * measured, all four at tech 7 by year 400 in every seed — after which research did nothing
 * for the remaining centuries.
 *
 * This is a real graph: thirty-six techs over nine eras, each with prerequisites, held as a
 * BITMASK so a gate is one AND. Kingdoms diverge because they pick what they need — a besieged
 * one takes gunpowder, a rich coastal one takes navigation and banking — and the tree is deep
 * enough that reaching the end is an achievement rather than a certainty.
 *
 * It ends where it has to end. A kingdom that reaches FISSION and ROCKETRY can build a silo,
 * and a kingdom with a silo that is losing a war will use it.
 */
enum {
    /* --- stone --- */
    TECH_NONE = 0, TECH_TOOLS, TECH_AGRICULTURE, TECH_POTTERY,
    /* --- bronze --- */
    TECH_MASONRY, TECH_BRONZE, TECH_WHEEL, TECH_WRITING,
    /* --- iron --- */
    TECH_IRON, TECH_SEAFARING, TECH_ARCHITECTURE, TECH_CURRENCY,
    /* --- classical --- */
    TECH_ENGINEER, TECH_MATHS, TECH_CAVALRY, TECH_LAW,
    /* --- medieval --- */
    TECH_GUNPOWDER, TECH_NAVIGATION, TECH_BANKING, TECH_UNIVERSITY,
    /* --- renaissance --- */
    TECH_PRINTING, TECH_METALLURGY, TECH_SANITATION, TECH_ECONOMICS,
    /* --- industrial --- */
    TECH_STEAM, TECH_RAILWAY, TECH_CHEMISTRY, TECH_ELECTRIC,
    /* --- modern --- */
    TECH_COMBUSTION, TECH_FLIGHT, TECH_RADIO, TECH_MEDICINE,
    /* --- atomic --- */
    TECH_PHYSICS, TECH_FISSION, TECH_ROCKETRY, TECH_NUKE,
    TECH_N
};
typedef char mb_tech_fits_a_mask[TECH_N <= 64 ? 1 : -1];   /* Kingdom.known is a uint64 */
#define TECHB(t) ((uint64_t)1u << (t))

enum { ERA_STONE = 0, ERA_BRONZE, ERA_IRON, ERA_CLASSICAL, ERA_MEDIEVAL,
       ERA_RENAISSANCE, ERA_INDUSTRIAL, ERA_MODERN, ERA_ATOMIC, ERA_N };

typedef struct {
    const char *name;
    uint8_t     era;
    uint16_t    cost;        /* research points */
    uint8_t     need[2];     /* prerequisites; TECH_NONE = none */
} TechDef;
extern const TechDef MB_TECH[TECH_N];
extern const char *const MB_ERA_NAME[ERA_N];
/* Kept for the HUD and the old call sites: the NAME of a tech id. */
extern const char *const MB_TECH_NAME[TECH_N];
enum { NEAR_PREY = 0, NEAR_MATE, NEAR_THREAT, NEAR_ENEMY };
enum { CAUSE_AGE = 0, CAUSE_WOUNDS, CAUSE_EATEN, CAUSE_SLAIN, CAUSE_DISASTER,
       CAUSE_PLAGUE, CAUSE_STARVED, CAUSE_N };

/* Traits. The first TR_RANDOM_N can be rolled at birth or inherited; the rest
 * are granted by the world or by the player and never appear at random — the
 * same split WorldBox draws. */
enum {
    TR_TOUGH_B = 0, TR_FAST_B, TR_BRAVE_B, TR_COWARD_B, TR_GREEDY_B, TR_LOYAL_B,
    TR_AMBITIOUS_B, TR_FERTILE_B, TR_BARREN_B, TR_GENIUS_B, TR_STUPID_B,
    TR_PIOUS_B, TR_VENGEFUL_B, TR_REGEN_B,
    TR_RANDOM_N,                                     /* --- above: inheritable */
    TR_BLESSED_B = TR_RANDOM_N, TR_CURSED_B, TR_CHOSEN_B, TR_MARKED_B,
    TR_PLAGUE_B, TR_MADNESS_B, TR_CONTAGIOUS_B, TR_ZOMBIE_B, TR_IMMORTAL_B,
    TR_VETERAN_B, TR_IMMUNE_B, TR_N
};
#define TRB(n) ((uint32_t)1u << (n))
#define TR_TOUGH     TRB(TR_TOUGH_B)
#define TR_FAST      TRB(TR_FAST_B)
#define TR_BRAVE     TRB(TR_BRAVE_B)
#define TR_COWARD    TRB(TR_COWARD_B)
#define TR_GREEDY    TRB(TR_GREEDY_B)
#define TR_LOYAL     TRB(TR_LOYAL_B)
#define TR_AMBITIOUS TRB(TR_AMBITIOUS_B)
#define TR_FERTILE   TRB(TR_FERTILE_B)
#define TR_BARREN    TRB(TR_BARREN_B)
#define TR_GENIUS    TRB(TR_GENIUS_B)
#define TR_STUPID    TRB(TR_STUPID_B)
#define TR_PIOUS     TRB(TR_PIOUS_B)
#define TR_VENGEFUL  TRB(TR_VENGEFUL_B)
#define TR_REGEN     TRB(TR_REGEN_B)
#define TR_BLESSED   TRB(TR_BLESSED_B)
#define TR_CURSED    TRB(TR_CURSED_B)
#define TR_CHOSEN    TRB(TR_CHOSEN_B)
#define TR_MARKED    TRB(TR_MARKED_B)
#define TR_PLAGUE    TRB(TR_PLAGUE_B)
#define TR_MADNESS   TRB(TR_MADNESS_B)
#define TR_CONTAGIOUS TRB(TR_CONTAGIOUS_B)
#define TR_ZOMBIE    TRB(TR_ZOMBIE_B)
#define TR_IMMORTAL  TRB(TR_IMMORTAL_B)
#define TR_VETERAN   TRB(TR_VETERAN_B)
#define TR_IMMUNE    TRB(TR_IMMUNE_B)

typedef struct {
    const char *name;
    uint8_t sheet;          /* 0 characters, 1 monsters, 2 animals */
    uint8_t cx, cy;         /* sprite cell in that sheet */
    uint8_t speed;          /* 1/16 tile per tick */
    uint8_t lifespan;       /* years */
    uint8_t diet;           /* DIET_* */
    uint8_t drives;         /* DRV_* */
} MbSpecies;
extern const MbSpecies MB_SP[SP_N];

/* Positions are 1/16 tile in uint16: 128 tiles * 16 = 2048, so a whole world
 * fits with sub-tile smoothness and no float anywhere in the sim. */
typedef struct {
    uint8_t  alive, sp, job, village;
    uint16_t x, y;
    int8_t   hp, happy;
    uint8_t  age, hunger;
    uint16_t target;        /* cell index, 0xFFFF = none */
    /* A PERSON, not a body. `family` alone meant every sibling in a village answered to
     * the same name and the inspect card could only say "a Thornwood" — so nobody was
     * anybody, and following one life was impossible. A given name, a trade, and a spouse
     * are what turn a moving sprite into somebody you can lose. */
    uint16_t family;        /* the surname, inherited from the parent */
    uint16_t given;         /* their own name */
    uint16_t mate;          /* unit index + 1 of their spouse; 0 = unwed */
    uint8_t  prof;          /* PROF_* — what they do, not what they are doing */
    uint8_t  dest;          /* a hauler's destination village; 0 = none */
    uint32_t traits;
    uint8_t  kills, carry, carry_kind;
    uint8_t  sick;          /* ticks of plague left to run; 0 = well */
} Unit;
extern Unit *mb_u;
extern int   mb_nu;         /* high-water mark; scans stop here */

/* --- civilisation -------------------------------------------------------
 * 48 villages and 12 kingdoms: enough for a continent to fragment several times
 * over, small enough that the kingdom brain's diplomacy pass is free and a
 * war_with bitmask fits in a uint32. */
#define MAXV 48
#define MAXK 12

enum { CARRY_FOOD = 0, CARRY_WOOD, CARRY_STONE, CARRY_IRON, CARRY_GOLD };

typedef struct {
    uint8_t  alive, sp, x, y;
    uint8_t  kingdom, hall, dirty, grace;
    uint16_t pop, housing;
    uint16_t food, wood, stone, iron, gold;
    int8_t   happy, loyalty;
    uint8_t  threat, exhaustion, unrest, mustering, soldiers;
    uint8_t  lord_diplo, lord_stew, lord_war;   /* the lord's three stats */
    uint32_t lord_traits;
    uint8_t  plan_obj, plan_x, plan_y, plan_i;  /* the blueprint ghost */
    int32_t  founded, last_settle;
    uint16_t name;
    uint16_t lord_name;     /* the lord is a person too, and lords are succeeded */
    uint8_t  lord_age;
    uint8_t  creed;         /* CREED_* — what this town believes */
    uint8_t  spec;          /* PROF_* — what the land around it makes it good at */
    uint8_t  coast;         /* water cells within reach; a quay needs some */
    uint8_t  tier;          /* TIER_*, recomputed each visit; cached so the HUD is cheap */
    uint16_t research;      /* accumulates toward the kingdom's next tech */
    uint16_t traded;        /* lifetime caravans received — a trade hub is a real thing */
} Village;

typedef struct {
    uint8_t  alive, sp, colour, capital;
    uint16_t pop;
    uint32_t war_with;      /* bit per kingdom */
    uint32_t ally_with;
    uint8_t  exhaustion, tech, creed, ntowns;   /* `tech` is now a COUNT known */
    uint64_t known;         /* TECHB() bitmask — the actual gate */
    uint8_t  era;           /* ERA_*, derived from what it knows */
    uint8_t  goal;          /* the tech it is working toward */
    uint8_t  nuked;         /* strikes it has launched, for the chronicle */
    uint16_t bank;          /* research banked toward `goal` */
    uint16_t name;
} Kingdom;

extern Village mb_v[MAXV];
extern Kingdom mb_k[MAXK];

/* --- world laws (DESIGN.md 12) ------------------------------------------ */
enum { LAW_DISASTER = 0, LAW_WAR, LAW_REBEL, LAW_PLAGUE, LAW_AGEING, LAW_BREED,
       LAW_MONSTERS, LAW_AGES, LAW_FOLLOW, LAW_TINT, LAW_NAMES, LAW_N };

/* --- the world ---------------------------------------------------------- */
typedef struct {
    uint8_t *biome;      /* B_* — the array the engine autotiles */
    uint8_t *elev;       /* 0..255; lava and water flow down it */
    uint8_t *obj;        /* O_* */
    uint8_t *flux;       /* kind:4 / intensity:4 — Phase 2 */
    uint8_t *claim;
    /* ROADS ARE THEIR OWN LAYER, not a biome. B_ROAD replaced the ground, so paving a
     * cell erased the grass or the field under it — and a terrain cell cannot be
     * transparent, so a road could never lie ON the land. One byte per cell (14 KB of
     * a 272 KB arena) buys a network that draws over any terrain and knows its own
     * neighbours, which is what a corner needs. */
    uint8_t *road;
    /* THE BAND MAP: one byte per cell, one bit per autotile layer, built CUMULATIVELY so
     * a cell in band B sets bits 0..B. This is what makes every terrain boundary in the
     * world automatic — see authoring/bands.py. Derived from biome[], not authored. */
    uint8_t *layer;      /* village id + development bits — Phase 4 */
    uint32_t seed;
    int32_t  tick;       /* weeks since year 0 */
    uint8_t  shape;      /* SH_* — archipelago .. pangaea, rolled from the seed */
    uint8_t  climate;    /* CL_* — frozen .. scorching */
    uint8_t  sea;        /* sea level in elev units; the shape sets it */
} World;
extern World mb_w;

#define AT(x, y) ((y) * MW + (x))
static inline int mb_in(int x, int y) { return x >= 0 && y >= 0 && x < MW && y < MH; }

/* mb_world.c */
void mb_world_alloc(void);
void mb_world_gen(uint32_t seed);
int  mb_sea_level(void);
void mb_world_start(int *x, int *y);   /* a sensible opening cursor cell */
const char *mb_world_shape_name(void);
const char *mb_world_climate_name(void);
void mb_world_stats(void);            /* MOTEBOX_STAT=1 */
void mb_grow_step(void);               /* regrowth, so a ruined world can heal */
void mb_grave_step(void);
void mb_bands_rebuild(void);           /* biome[] -> layer[], the cumulative band map */              /* headstones weather, so peace clears them */

/* mb_flux.c */
void mb_flux_init(void);
void mb_flux_reset(void);
void mb_flux_test_event(const char *name, uint32_t r);   /* MOTEBOX_EVENT= */
void mb_flux_step(void);
void mb_flux_add(int x, int y, int kind, int inten);
void mb_flux_blob(int cx, int cy, int r, int kind, int inten);
void mb_flux_ignite(int cx, int cy, int r);   /* fire, strength from the ground */
int  mb_flux_douse(int x, int y, int power);   /* beat it out; returns what was cut */
void mb_flux_wind(int *dx, int *dy);
int  mb_flux_wind_phase(void);
int  mb_flux_count(void);
void mb_agent_spawn(int kind, int x, int y);
int  mb_agent_count(void);
int  mb_agent_get(int i, int *x, int *y, int *kind);
int  mb_agent_max(void);
int  mb_agent_hurt(int x, int y, int dmg);
int  mb_agent_hp(int i);
void mb_flux_natural(void);

/* mb_fx.c */
void  mb_fx_init(void);
void  mb_fx_step(float dt);
void  mb_fx_spawn(float tx, float ty, int kind, int elem, float speed, float life);
void  mb_fx_burst(float tx, float ty, int n, int kind, int elem, float speed, float life);
void  mb_fx_impact(float tx, float ty, int elem, float power);
void  mb_fx_shake(float amp);
void  mb_fx_flash(float amt);
float mb_fx_shake_amt(void);
void  mb_fx_draw_god(uint16_t *fb);
void  mb_fx_flux_render(uint16_t *fb, int cam_x, int cam_y);
void  mb_fx_draw_mortal_px(uint16_t *fb, int cam_x, int cam_y);
void  mb_fx_flux_emit(int cam_x, int cam_y, float dt);
void  mb_fx_spawn_v(float tx, float ty, float vx, float vy, int kind, int elem, float life);

/* Deterministic per-(seed, tick, salt) randomness — the sim's only entropy, so
 * a world replays identically from its seed (DESIGN.md 5). */
static inline uint32_t mb_rand(uint32_t salt);

/* mb_power.c */
void        mb_power_cast(int cx, int cy);
int         mb_power_cast_named(const char *name, int cx, int cy);
#if MOTE_HOST
void        mb_power_test_all(int cx, int cy);
#endif
int         mb_power_input(const MoteInput *in);
void        mb_power_draw_wheel(uint16_t *fb, const MoteFont *font);
const char *mb_power_name(void);
const char *mb_power_tab(void);
int         mb_power_cost(void);
int         mb_power_radius(void);
int         mb_power_brush(void);
int         mb_power_last_reach(void);  /* people the last cast reached, -1 = n/a */
int         mb_wheel_open(void);

/* mb_unit.c */
void mb_unit_init(void);      /* allocs once */
void mb_unit_reset(void);     /* a new world: no allocation, the arena has no free */
void mb_unit_step(void);
int  mb_unit_spawn(int sp, int x, int y);
void mb_unit_kill(int i, int cause);
int  mb_unit_passable(int sp, int x, int y);
int  mb_unit_nearest(int self, int x, int y, int mode);
void mb_unit_seed_wildlife(void);
/* One pass over the units in a disc — the shape every unit-targeting power and
 * every area effect needs, so none of them re-walks the array itself. */
enum { UAP_HEAL = 0, UAP_HURT, UAP_TRAIT, UAP_UNTRAIT, UAP_STARVE, UAP_KILL,
       UAP_HAPPY, UAP_RAGE };
int  mb_unit_area(int cx, int cy, int r, int op, uint32_t arg);
int  mb_unit_at(int x, int y);
void mb_unit_plague_step(void);
void mb_unit_madness_step(void);
int  mb_unit_raise_dead(int cx, int cy, int r);
void mb_unit_migrate(void);   /* a trickle from off-map: extinction must not be absorbing */
void mb_unit_recount(void);   /* after a load: the counters live outside the array */
int  mb_pop(int sp);
int  mb_pop_civ(void);
int  mb_pop_all(void);
int  mb_pop_wild(void);
int  mb_pop_class_full(int sp);
int  mb_births(void);
int  mb_deaths(void);
int  mb_deaths_by(int cause);   /* civ deaths by CAUSE_*, for the audit */

/* mb_civ.c */
void mb_civ_init(void);
void mb_civ_reset(void);
void mb_civ_step(void);
int  mb_civ_drop_village(int sp, int x, int y);
void mb_civ_seed_world(int n);         /* worlds start with peoples in them */
void mb_civ_rehome(void);              /* the homeless join a village or found one */
int  mb_civ_count(int v, uint8_t obj);   /* how many of `obj` this village has */
extern int mb_mined_iron, mb_mined_gold;  /* lifetime mine yield */
int  mb_civ_tier(int v);               /* TIER_* from pop and what is standing */
int  mb_civ_tech_ok(int v, int need);  /* does this village's kingdom know `need`? */
int  mb_tech_known(int k, int t);      /* by kingdom */
int  mb_tech_avail(int k, int t);      /* prerequisites met and not yet known */
void mb_nuke_strike(int from_k, int x, int y);
/* WAR PROJECTILES, in the order the tech tree grants them. The kind dates the battle. */
enum { MB_SHOT_ROCK = 0, MB_SHOT_ARROW, MB_SHOT_BULLET, MB_SHOT_SHELL, MB_SHOT_MISSILE,
       MB_SHOT_N };
void mb_fx_shot(float fx, float fy, float tx, float ty, int kind);
void mb_fx_draw_shots(uint16_t *fb, int cam_x, int cam_y, float dt);
int  mb_fx_shots_live(void);
void mb_fx_shots_clear(void);
int  mb_war_shot_kind(int k);        /* what this kingdom's soldiers fire */
int  mb_war_shot_range(int kind);    /* how far, in tiles */
void mb_fx_nuke(int cx, int cy);                 /* start a mushroom cloud */
void mb_fx_draw_nuke(uint16_t *fb, int cam_x, int cam_y, float dt);
void mb_fx_nuke_clear(void);
void mb_civ_specialise(int v);         /* what the surrounding land makes it good at */
int  mb_civ_prof_pick(int v);          /* the trade a newborn of this village takes up */
void mb_civ_deliver(int v, int kind, int amount);   /* a caravan arrives */
int  mb_village_found(int sp, int x, int y, int kingdom);
int  mb_village_need(int v, uint16_t *target);
int  mb_village_resource(int v, int kind, int *ox, int *oy);
void mb_village_work(int v, int ui);
int  mb_village_mustering(int v);
int  mb_village_step_home(int v, int x, int y, int *ox, int *oy);
int  mb_village_count(void);
int  mb_village_count_last(void);
int  mb_kingdom_of(int v);
int  mb_kingdom_count(void);
int  mb_at_war(int a, int b);
int  mb_border_len(int a, int b);

/* mb_chron.c — names, events, legends: the story engine (DESIGN.md 9) */
void        mb_chron_init(void);
uint16_t    mb_name_place(uint32_t salt);
uint16_t    mb_name_kingdom(uint32_t salt);
uint16_t    mb_name_person(uint32_t salt);
void        mb_name_str(char *out, int n, int kind, uint16_t id);
void        mb_chron_found(int v);
void        mb_chron_fall(int v);
void        mb_chron_build(int v, const char *what);
void        mb_chron_war(int a, int b);
void        mb_chron_peace(int a, int b);
void        mb_chron_rebel(int v, int from, int to);
void        mb_chron_birth(int child, int parent);
void        mb_chron_death(int u, int cause);
void        mb_chron_legend(int u, int why);
void        mb_chron_disaster(const char *what, int x, int y);
void        mb_chron_age(const char *name);
const char *mb_chron_word(int id);
int         mb_chron_grudge(int a, int b);
int         mb_chron_count(void);
int         mb_chron_notable(int back);  /* worth a line in the log? */
void        mb_chron_line(char *out, int n, int back, int *year);
int         mb_chron_where(int back, int *x, int *y);   /* jump to an entry */
const char *mb_chron_toast(void);      /* NULL when none is showing */
const char *mb_chron_headline(void);   /* the last one, no timer */
void        mb_chron_step(float dt);
int         mb_chron_focus(int *x, int *y);   /* where the last big event was */
enum { LEGEND_KILLS = 0, LEGEND_KING, LEGEND_SURVIVOR, LEGEND_FOUNDER, LEGEND_N };

/* mb_age.c — the eight ages and the world laws */
enum { AGE_HOPE = 0, AGE_SUN, AGE_MOON, AGE_IRON, AGE_CHAOS, AGE_ICE, AGE_ASH,
       AGE_DESPAIR, AGE_N };
void        mb_age_init(void);
void        mb_age_step(void);
const char *mb_age_name(void);
int         mb_age_id(void);
int         mb_age_war_bias(void);
int         mb_age_allows_armies(void);
int         mb_age_fertility(void);
int         mb_age_dryness(void);
int         mb_age_cold(void);
int         mb_age_faith_mod(void);
uint16_t    mb_age_tint(int *amt);
int         mb_law(int which);
void        mb_law_toggle(int which);
uint16_t    mb_law_bits(void);
void        mb_law_set_bits(uint16_t b);
void        mb_age_set(int a);
const char *mb_law_name(int which);

/* mb_faith.c — Faith, and the two modes (DESIGN.md 11) */
enum { MODE_PANTHEON = 0, MODE_SANDBOX };
void mb_faith_init(void);
void mb_faith_step(void);
int  mb_faith(void);
int  mb_faith_income(void);
int  mb_faith_afford(int cost);
void mb_faith_spend(int cost);
void mb_faith_set(int v);
int  mb_mode(void);
void mb_mode_set(int m);

/* mb_save.c */
void mb_save_init(void);
int  mb_save_write(int slot, int cx, int cy, int god);
int  mb_save_read(int slot, int *cx, int *cy, int *god);
int  mb_save_exists(int slot);

/* mb_audio.c — restraint by design: only headline events make a noise, each
 * rate-limited, because a world at x8 runs a year in under a second. */
enum { SND_FIRE = 0, SND_BOOM, SND_QUAKE, SND_THUNDER, SND_SPLASH, SND_FREEZE,
       SND_BUILD, SND_FOUND, SND_WAR, SND_FALL, SND_TITAN, SND_BLESS,
       SND_CURSE, SND_DENY, SND_AGE, SND_N };
void mb_audio_step(float dt);
void mb_snd(int id);
void mb_snd_at(int id, int x, int y, int cam_tx, int cam_ty, int radius);

/* mb_draw.c */
void mb_draw_init(void);
/* GOD'S EYE MAP MODES — the same political map keyed by a different fact. */
enum { MAPMODE_POWER = 0, MAPMODE_FAITH, MAPMODE_CRAFT, MAPMODE_GROWTH, MAPMODE_N };
extern const char *const MB_MAPMODE_NAME[MAPMODE_N];
void mb_god_towns(uint16_t *fb, int y0, int y1);   /* settlement marks on the world map */
int  mb_draw_mapmode(void);
void mb_draw_mapmode_set(int m);
void mb_draw_prepare(void);   /* rebuilds the tint LUT when an age or kingdom changes */
void mb_god_band(uint16_t *fb, int y0, int y1);      /* set_background_cb target */
void mb_god_units(uint16_t *fb, int y0, int y1);
uint16_t mb_kingdom_colour(int k);
void mb_dim_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t toward, int amt);
void mb_draw_sprite_load(int *want, int *lost);
int  mb_draw_form_col(uint8_t o, int k, int c, int r); /* the column it draws as */
void mb_draw_sea_band(uint16_t *fb, int y0, int y1);
void mb_draw_mortal_sprites(uint16_t *fb);
void mb_draw_relief(uint16_t *fb, int cam_x, int cam_y);
void mb_draw_mortal(int cam_x, int cam_y);           /* scene2d pass */
uint16_t mb_biome_colour(uint8_t b);

/* defined here rather than in a .c so the whole sim inlines it */
static inline uint32_t mb_rand(uint32_t salt)
{
    uint32_t x = mb_w.seed ^ ((uint32_t)mb_w.tick * 2654435761u) ^ salt;
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

#endif /* MB_H */
