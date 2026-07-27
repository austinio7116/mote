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

/* mb_draw.c */
void mb_draw_init(void);
void mb_god_band(uint16_t *fb, int y0, int y1);      /* set_background_cb target */
void mb_draw_mortal(int cam_x, int cam_y);           /* scene2d pass */
uint16_t mb_biome_colour(uint8_t b);

#endif /* MB_H */
