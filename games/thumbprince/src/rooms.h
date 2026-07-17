/* ThumbPrince room catalogue — shapes, economy, interiors.
 *
 * Interiors are 7x7 tile templates (16px tiles): the border ring renders from
 * the walls_* rule tilesets, floors from floors.png macro-tiles, furniture as
 * prop sprites (props_meta.h) anchored by template letters. Loot is placed on
 * open floor at runtime from the day seed.
 *
 * Template chars:
 *   '#' wall ring    '.' floor
 *   props: u bush  C campfire  L shelf_big  l shelf_small
 *          b bed_blue  B bed_red  t table  h chair  s sofa  K counter
 *          S stove  U tub  T toilet  d desk  m map_table  W washer
 *          r barrel  g gold_pile  w workbench  p plant
 *   chests (interactive, bump to open): c chest  x PADLOCKED chest (1 key)
 * Every template must keep the four door approaches walkable:
 * tiles (3,1) (5,3) (3,5) (1,3) — any side can be the entry.
 */
#ifndef DRAFT_ROOMS_H
#define DRAFT_ROOMS_H

#include <stdint.h>

enum { DIR_N = 0, DIR_E, DIR_S, DIR_W };
#define DBIT(d) (1u << (d))

/* door shapes, relative to the door you entered by */
enum { SH_DEAD = 0, SH_STR, SH_L, SH_R, SH_T, SH_X };

enum {
    RF_GREEN = 1,        /* garden room: +25 pts per adjacent green at placement */
    RF_UNIQUE = 2,       /* at most one per day */
    RF_SHOP_COM = 4,     /* commissary stock */
    RF_SHOP_LOCK = 8,    /* locksmith stock */
    RF_LOCKED = 16,      /* costs 1 key to draft (the Vault) */
    RF_COMPASS = 32,     /* first entry grants the Compass (rotate drafts) */
    RF_PENCIL = 64,      /* first entry grants the Pencil (gem rerolls) */
};

enum {                   /* loot pickup types */
    IT_NONE = 0, IT_COIN, IT_KEY, IT_GEM, IT_FOOD, IT_STAR, IT_STAR2, IT_STAR3,
    IT_POTION,           /* tonic: +10 steps */
    IT_POUCH,            /* gold pouch: +3 gold */
};

/* floors.png macro-tile columns */
enum {
    FL_WOOD = 0, FL_STONE, FL_RED, FL_BLUE, FL_GRASS, FL_CHECKER,
    FL_WOOD_DARK, FL_LEAFY, FL_AUTUMN,
};
enum { WL_STONE = 0, WL_RED, WL_DARK };      /* walls_* rule sheets */

typedef struct {
    const char *name;               /* fits the draft card: keep <= 11 chars */
    uint8_t shape, rarity;          /* rarity 0 common / 1 uncommon / 2 rare */
    uint8_t gems;                   /* gem cost to draft */
    uint8_t floor, wall;
    uint16_t map_col;               /* estate map colour (RGB565) */
    uint8_t flags;
    int8_t  steps, keys, gemsg;     /* first-entry gains */
    uint16_t pts;                   /* first-entry score */
    const char *tmpl;               /* 7 rows x 7 cols */
    uint8_t loot[6];                /* pickups, auto-placed on open floor */
} RoomDef;

enum {
    R_ENTRANCE = 0, R_ANTE,
    R_HALLWAY, R_WPASS, R_EPASS, R_FOYER, R_GREATHALL,
    R_LOUNGE, R_DRAWING, R_DINING, R_KITCHEN, R_PANTRY,
    R_BEDROOM, R_SUITE, R_WASHROOM, R_LIBRARY, R_STUDY, R_DRAFTING,
    R_LAUNDRY, R_STORE, R_CELLAR, R_LOCKSMITH, R_COMMISSARY,
    R_HEARTH, R_STILLROOM,
    R_TERRACE, R_GARDEN, R_SUNROOM, R_VAULT,
    R_GUEST, R_CHAPEL, R_ARMORY, R_WINECELLAR, R_ORCHARD, R_TREASURY,
    R_NOOK, R_SCULLERY, R_SOLARIUM, R_GAMES, R_BUNK, R_ATELIER,
    R_COUNT,
};

#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const RoomDef k_rooms[R_COUNT] = {
    [R_ENTRANCE] = { "ENTRANCE", SH_X, 0, 0, FL_WOOD, WL_STONE, C565(96, 122, 176), 0, 0, 0, 0, 0,
        "#######"
        "#p...p#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_KEY, IT_COIN, 0 } },

    [R_ANTE] = { "ANTECHAMBER", SH_X, 2, 0, FL_RED, WL_STONE, C565(236, 198, 96), RF_UNIQUE, 0, 0, 0, 0,
        "#######"
        "#p...p#"
        "#.....#"
        "#.....#"
        "#g...g#"
        "#.....#"
        "#######", { 0 } },

    [R_HALLWAY] = { "HALLWAY", SH_STR, 0, 0, FL_WOOD, WL_STONE, C565(168, 126, 80), 0, 0, 0, 0, 0,
        "#######"
        "#.....#"
        "#.....#"
        "#.....#"
        "#p....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_WPASS] = { "WEST WALK", SH_L, 0, 0, FL_WOOD, WL_STONE, C565(156, 116, 74), 0, 0, 0, 0, 0,
        "#######"
        "#....p#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_EPASS] = { "EAST WALK", SH_R, 0, 0, FL_WOOD, WL_STONE, C565(156, 116, 74), 0, 0, 0, 0, 0,
        "#######"
        "#p....#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_FOYER] = { "FOYER", SH_T, 1, 1, FL_WOOD, WL_STONE, C565(146, 104, 66), 0, 0, 0, 0, 0,
        "#######"
        "#p...p#"
        "#.....#"
        "#.....#"
        "#h....#"
        "#.....#"
        "#######", { IT_COIN, IT_COIN, 0 } },

    [R_GREATHALL] = { "GREAT HALL", SH_X, 2, 2, FL_RED, WL_STONE, C565(190, 150, 96), RF_UNIQUE, 0, 0, 0, 50,
        "#######"
        "#p...p#"
        "#.....#"
        "#.....#"
        "#p...p#"
        "#.....#"
        "#######", { IT_COIN, IT_COIN, 0 } },

    [R_LOUNGE] = { "LOUNGE", SH_DEAD, 0, 0, FL_WOOD, WL_STONE, C565(170, 62, 58), 0, 6, 0, 0, 0,
        "#######"
        "#s....#"
        "#.....#"
        "#.....#"
        "#t...p#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_DRAWING] = { "DRAWING RM", SH_L, 1, 0, FL_BLUE, WL_STONE, C565(96, 150, 96), 0, 0, 0, 1, 25,
        "#######"
        "#l...p#"
        "#.....#"
        "#.....#"
        "#s....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_DINING] = { "DINING RM", SH_STR, 0, 0, FL_WOOD, WL_STONE, C565(80, 110, 170), 0, 4, 0, 0, 0,
        "#######"
        "#.....#"
        "#ht..h#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_KITCHEN] = { "KITCHEN", SH_R, 0, 0, FL_CHECKER, WL_RED, C565(206, 196, 176), 0, 4, 0, 0, 0,
        "#######"
        "#K...S#"
        "#.....#"
        "#.....#"
        "#....p#"
        "#.....#"
        "#######", { IT_FOOD, 0 } },

    [R_PANTRY] = { "PANTRY", SH_DEAD, 0, 0, FL_WOOD_DARK, WL_RED, C565(108, 108, 124), 0, 4, 0, 0, 0,
        "#######"
        "#l...r#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#r...r#"
        "#######", { IT_FOOD, IT_COIN, 0 } },

    [R_BEDROOM] = { "BEDROOM", SH_DEAD, 0, 0, FL_BLUE, WL_STONE, C565(90, 128, 186), 0, 6, 0, 0, 0,
        "#######"
        "#b...p#"
        "#.....#"
        "#.....#"
        "#....h#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_SUITE] = { "SUITE", SH_DEAD, 2, 1, FL_RED, WL_STONE, C565(150, 96, 190), 0, 12, 1, 0, 0,
        "#######"
        "#B...c#"
        "#.....#"
        "#.....#"
        "#....p#"
        "#.....#"
        "#######", { IT_COIN, IT_STAR, 0 } },

    [R_WASHROOM] = { "WASHROOM", SH_DEAD, 0, 0, FL_CHECKER, WL_STONE, C565(140, 190, 226), 0, 4, 0, 0, 0,
        "#######"
        "#p...T#"
        "#.....#"
        "#.....#"
        "#U....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_LIBRARY] = { "LIBRARY", SH_STR, 1, 0, FL_WOOD, WL_STONE, C565(134, 96, 60), 0, 0, 0, 0, 0,
        "#######"
        "#L..l.#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_STAR2, IT_COIN, 0 } },

    [R_STUDY] = { "STUDY", SH_L, 1, 0, FL_WOOD, WL_STONE, C565(64, 120, 84), RF_PENCIL, 0, 0, 1, 50,
        "#######"
        "#d...l#"
        "#.....#"
        "#.....#"
        "#....p#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_DRAFTING] = { "DRAFT ROOM", SH_DEAD, 1, 1, FL_WOOD, WL_STONE, C565(214, 190, 150), RF_UNIQUE | RF_COMPASS, 0, 0, 2, 0,
        "#######"
        "#l....#"
        "#..m..#"
        "#.....#"
        "#p....#"
        "#.....#"
        "#######", { IT_GEM, 0 } },

    [R_LAUNDRY] = { "LAUNDRY", SH_DEAD, 0, 0, FL_STONE, WL_STONE, C565(214, 218, 226), 0, 0, 0, 0, 0,
        "#######"
        "#W...W#"
        "#.....#"
        "#.....#"
        "#r....#"
        "#.....#"
        "#######", { IT_KEY, 0 } },

    [R_STORE] = { "STOREROOM", SH_DEAD, 1, 1, FL_WOOD_DARK, WL_RED, C565(160, 60, 66), 0, 0, 0, 0, 0,
        "#######"
        "#c...r#"
        "#.....#"
        "#.....#"
        "#r..c.#"
        "#.....#"
        "#######", { IT_COIN, IT_KEY, IT_STAR, 0 } },

    [R_CELLAR] = { "CELLAR", SH_R, 1, 1, FL_STONE, WL_DARK, C565(120, 124, 136), RF_UNIQUE, 0, 0, 0, 0,
        "#######"
        "#r...r#"
        "#.....#"
        "#.....#"
        "#x....#"
        "#.....#"
        "#######", { IT_KEY, IT_KEY, 0 } },

    [R_LOCKSMITH] = { "LOCKSMITH", SH_DEAD, 1, 0, FL_STONE, WL_STONE, C565(96, 100, 116), RF_UNIQUE | RF_SHOP_LOCK, 0, 0, 0, 0,
        "#######"
        "#w...l#"
        "#.....#"
        "#.....#"
        "#....r#"
        "#.....#"
        "#######", { IT_KEY, 0 } },

    [R_COMMISSARY] = { "COMMISSARY", SH_STR, 1, 0, FL_WOOD, WL_STONE, C565(216, 168, 92), RF_UNIQUE | RF_SHOP_COM, 0, 0, 0, 0,
        "#######"
        "#K...l#"
        "#.....#"
        "#.....#"
        "#....p#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_HEARTH] = { "HEARTH", SH_STR, 1, 0, FL_WOOD, WL_RED, C565(226, 130, 54), 0, 8, 0, 0, 0,
        "#######"
        "#s....#"
        "#.....#"
        "#..C..#"
        "#.....#"
        "#.....#"
        "#######", { 0 } },

    [R_STILLROOM] = { "STILL ROOM", SH_DEAD, 2, 2, FL_WOOD_DARK, WL_DARK, C565(52, 64, 120), RF_UNIQUE, 0, 0, 0, 0,
        "#######"
        "#l...r#"
        "#.....#"
        "#.....#"
        "#..t..#"
        "#.....#"
        "#######", { IT_POTION, IT_POTION, IT_STAR, 0 } },

    [R_TERRACE] = { "TERRACE", SH_L, 0, 0, FL_GRASS, WL_STONE, C565(96, 160, 70), RF_GREEN, 0, 0, 1, 0,
        "#######"
        "#u....#"
        "#.....#"
        "#.....#"
        "#....u#"
        "#.....#"
        "#######", { IT_GEM, 0 } },

    [R_GARDEN] = { "GARDEN", SH_DEAD, 1, 0, FL_LEAFY, WL_STONE, C565(80, 150, 60), RF_GREEN, 0, 0, 1, 0,
        "#######"
        "#u...u#"
        "#.....#"
        "#.....#"
        "#p....#"
        "#.....#"
        "#######", { IT_GEM, IT_COIN, 0 } },

    [R_SUNROOM] = { "SUNROOM", SH_T, 1, 1, FL_GRASS, WL_STONE, C565(70, 130, 74), RF_GREEN, 0, 0, 2, 0,
        "#######"
        "#u...u#"
        "#.....#"
        "#.....#"
        "#p...p#"
        "#.....#"
        "#######", { IT_GEM, IT_GEM, 0 } },

    [R_VAULT] = { "VAULT", SH_DEAD, 2, 2, FL_STONE, WL_DARK, C565(240, 205, 80), RF_UNIQUE | RF_LOCKED, 0, 0, 0, 0,
        "#######"
        "#g...g#"
        "#..x..#"
        "#.....#"
        "#g...g#"
        "#.....#"
        "#######", { IT_COIN, IT_COIN, IT_COIN, IT_COIN, IT_COIN, IT_STAR } },

    [R_GUEST] = { "GUEST ROOM", SH_DEAD, 0, 0, FL_BLUE, WL_STONE, C565(110, 140, 196), 0, 4, 0, 0, 0,
        "#######"
        "#b....#"
        "#.....#"
        "#.....#"
        "#c...p#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_CHAPEL] = { "CHAPEL", SH_STR, 1, 0, FL_BLUE, WL_STONE, C565(120, 110, 200), 0, 0, 0, 0, 0,
        "#######"
        "#C...C#"
        "#..t..#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_STAR2, IT_COIN, 0 } },

    [R_ARMORY] = { "ARMORY", SH_R, 1, 1, FL_STONE, WL_DARK, C565(140, 140, 156), 0, 0, 1, 0, 0,
        "#######"
        "#w...r#"
        "#.....#"
        "#.....#"
        "#x....#"
        "#.....#"
        "#######", { IT_KEY, IT_COIN, 0 } },

    [R_WINECELLAR] = { "WINE STORE", SH_DEAD, 1, 0, FL_WOOD_DARK, WL_DARK, C565(140, 70, 110), 0, 0, 0, 0, 0,
        "#######"
        "#r...r#"
        "#.....#"
        "#.....#"
        "#r...r#"
        "#.....#"
        "#######", { IT_POTION, IT_POUCH, IT_COIN, 0 } },

    [R_ORCHARD] = { "ORCHARD", SH_L, 1, 0, FL_LEAFY, WL_STONE, C565(120, 160, 54), RF_GREEN, 0, 0, 1, 0,
        "#######"
        "#u...u#"
        "#.....#"
        "#.....#"
        "#....u#"
        "#.....#"
        "#######", { IT_FOOD, IT_GEM, 0 } },

    [R_TREASURY] = { "TREASURY", SH_DEAD, 2, 2, FL_RED, WL_DARK, C565(230, 170, 60), RF_UNIQUE, 0, 0, 0, 0,
        "#######"
        "#x...x#"
        "#.....#"
        "#.....#"
        "#..g..#"
        "#.....#"
        "#######", { IT_POUCH, IT_COIN, 0 } },

    [R_NOOK] = { "NOOK", SH_DEAD, 0, 0, FL_WOOD, WL_STONE, C565(150, 110, 70), 0, 0, 0, 0, 25,
        "#######"
        "#h...l#"
        "#.....#"
        "#.....#"
        "#....p#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_SCULLERY] = { "SCULLERY", SH_R, 0, 0, FL_CHECKER, WL_STONE, C565(190, 198, 206), 0, 2, 0, 0, 0,
        "#######"
        "#W...K#"
        "#.....#"
        "#.....#"
        "#r....#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_SOLARIUM] = { "SOLARIUM", SH_STR, 1, 0, FL_GRASS, WL_STONE, C565(110, 170, 90), RF_GREEN, 0, 0, 1, 0,
        "#######"
        "#p...p#"
        "#.....#"
        "#.....#"
        "#s....#"
        "#.....#"
        "#######", { IT_GEM, 0 } },

    [R_GAMES] = { "GAMES ROOM", SH_L, 1, 0, FL_RED, WL_STONE, C565(180, 120, 60), 0, 0, 0, 0, 25,
        "#######"
        "#h...h#"
        "#..t..#"
        "#.....#"
        "#.....#"
        "#.....#"
        "#######", { IT_COIN, IT_STAR, 0 } },

    [R_BUNK] = { "BUNK ROOM", SH_DEAD, 0, 0, FL_WOOD_DARK, WL_STONE, C565(100, 118, 160), 0, 8, 0, 0, 0,
        "#######"
        "#b...b#"
        "#.....#"
        "#.....#"
        "#....r#"
        "#.....#"
        "#######", { IT_COIN, 0 } },

    [R_ATELIER] = { "ATELIER", SH_DEAD, 2, 1, FL_WOOD, WL_STONE, C565(210, 160, 190), RF_UNIQUE | RF_PENCIL, 0, 0, 1, 0,
        "#######"
        "#d...p#"
        "#.....#"
        "#.....#"
        "#..m..#"
        "#.....#"
        "#######", { IT_STAR2, IT_GEM, 0 } },
};

/* draftable room ids (everything but the two fixed rooms) */
#define DRAFT_FIRST R_HALLWAY
static const uint8_t k_rarity_weight[3] = { 6, 3, 1 };

#endif
