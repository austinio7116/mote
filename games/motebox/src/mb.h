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

/* --- objects: one per cell ---------------------------------------------- */
enum {
    O_NONE = 0,
    O_TREE, O_TREE2, O_DEAD, O_BUSH, O_TUFT, O_ROCK, O_CACTUS, O_FLOWER,
    O_ORE, O_SILVER, O_GOLD, O_GEM, O_BOULDER, O_PEAKROCK,
    O_N
};

/* --- flux: the disaster channel ----------------------------------------
 * One byte per cell, kind:4 / intensity:4. Every disaster in DESIGN.md 10 is a
 * case in mb_flux_step()'s switch, so they share spread, decay, wind and the
 * terrain consequences instead of each owning a system. */
enum { FX_NONE = 0, FX_FIRE, FX_LAVA, FX_WATER, FX_ACID, FX_FROST, FX_N };
/* disasters that WALK rather than spread: a place that keeps acting for a while */
enum { AG_TORNADO = 0, AG_VENT, AG_N };
static inline uint8_t mb_fkind(uint8_t f) { return (uint8_t)(f >> 4); }
static inline uint8_t mb_fint(uint8_t f)  { return (uint8_t)(f & 15); }

/* FX sheets: the six elemental recolours of the master's mono FX band. */
enum { FXE_FIRE = 0, FXE_FROST, FXE_ACID, FXE_ASH, FXE_HOLY, FXE_VOID, FXE_N };
/* particle shapes, each a cell run in those sheets */
enum { PK_SPARK = 0, PK_SMOKE, PK_RING, PK_BOLT, PK_GUST, PK_STAR, PK_N };

/* --- the world ---------------------------------------------------------- */
typedef struct {
    uint8_t *biome;      /* B_* — the array the engine autotiles */
    uint8_t *elev;       /* 0..255; lava and water flow down it */
    uint8_t *obj;        /* O_* */
    uint8_t *flux;       /* kind:4 / intensity:4 — Phase 2 */
    uint8_t *claim;      /* village id + development bits — Phase 4 */
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

/* mb_flux.c */
void mb_flux_init(void);
void mb_flux_step(void);
void mb_flux_add(int x, int y, int kind, int inten);
void mb_flux_blob(int cx, int cy, int r, int kind, int inten);
void mb_flux_ignite(int cx, int cy, int r);   /* fire, strength from the ground */
void mb_flux_wind(int *dx, int *dy);
int  mb_flux_wind_phase(void);
int  mb_flux_count(void);
void mb_agent_spawn(int kind, int x, int y);
int  mb_agent_count(void);
int  mb_agent_get(int i, int *x, int *y, int *kind);
int  mb_agent_max(void);

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
void  mb_fx_draw_mortal(int cam_x, int cam_y);

/* Deterministic per-(seed, tick, salt) randomness — the sim's only entropy, so
 * a world replays identically from its seed (DESIGN.md 5). */
static inline uint32_t mb_rand(uint32_t salt);

/* mb_power.c */
void        mb_power_cast(int cx, int cy);
int         mb_power_cast_named(const char *name, int cx, int cy);
int         mb_power_input(const MoteInput *in);
void        mb_power_draw_wheel(uint16_t *fb, const MoteFont *font);
const char *mb_power_name(void);
const char *mb_power_tab(void);
int         mb_power_cost(void);
int         mb_power_radius(void);
int         mb_power_brush(void);
int         mb_wheel_open(void);

/* mb_draw.c */
void mb_draw_init(void);
void mb_god_band(uint16_t *fb, int y0, int y1);      /* set_background_cb target */
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
