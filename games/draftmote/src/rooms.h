/* DraftMote room catalogue — shapes, economy, and walkable interiors.
 *
 * Interiors are 8x7 tile templates (16px tiles). Chars:
 *   '#' wall   '.' floor   digits 0-5 = loot slot (item from RoomDef.loot)
 *   props: c chest  C open chest  t table  h chair  b bed  B bookshelf
 *          p plant  x crate  f fountain  a altar  o telescope  s stove
 *          r barrel  g gem rock  R rug (walkable)  A anvil
 *          K shop counter (bump to shop)  S shelf wall
 * Doorways are carved at N(3,0) E(7,3) S(3,6) W(0,3) from the door mask, so
 * every template must keep (3,1) (6,3) (3,5) (1,3) walkable.
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
};

enum {                   /* loot pickup types */
    IT_NONE = 0, IT_COIN, IT_KEY, IT_GEM, IT_FOOD, IT_STAR, IT_STAR2, IT_STAR3,
};

typedef struct {
    const char *name;
    uint8_t shape, rarity;          /* rarity 0 common / 1 uncommon / 2 rare */
    uint8_t floor, wall;            /* tiles.png column: floors row 0, walls row 1 */
    uint8_t gems;                   /* gem cost to draft */
    uint16_t map_col;               /* estate map colour (RGB565) */
    uint8_t flags;
    int8_t  steps, keys, gemsg;     /* first-entry gains */
    uint16_t pts;                   /* first-entry score */
    const char *tmpl;               /* 7 rows x 8 cols */
    uint8_t loot[6];
} RoomDef;

enum {
    R_ENTRANCE = 0, R_ANTE,
    R_HALLWAY, R_WPASS, R_EPASS, R_DINING, R_KITCHEN, R_BEDROOM, R_CLOSET,
    R_PANTRY, R_TERRACE, R_GARDEN, R_FOYER, R_STUDY, R_CHAPEL, R_STORE,
    R_GEMDEN, R_SECURITY, R_COMMISSARY, R_LOCKSMITH, R_CONSERV, R_BALLROOM,
    R_SUITE, R_VAULT, R_OBSERV, R_GREATHALL,
    R_COUNT,
};

#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const RoomDef k_rooms[R_COUNT] = {
    [R_ENTRANCE] = { "ENTRANCE HALL", SH_X, 0, 0, 1, 0, C565(150, 106, 62), 0, 0, 0, 0, 0,
        "########"
        "#p.....#"
        "#..0...#"
        "#...R..#"
        "#......#"
        "#..1..p#"
        "########", { IT_COIN, 0 } },

    [R_ANTE] = { "ANTECHAMBER", SH_X, 2, 6, 5, 0, C565(230, 196, 90), RF_UNIQUE, 0, 0, 0, 0,
        "########"
        "#..a...#"
        "#......#"
        "#......#"
        "#......#"
        "#......#"
        "########", { 0 } },

    [R_HALLWAY] = { "HALLWAY", SH_STR, 0, 0, 0, 0, C565(170, 130, 84), 0, 0, 0, 0, 0,
        "########"
        "#......#"
        "#.x....#"
        "#...0..#"
        "#....x.#"
        "#......#"
        "########", { IT_COIN, 0 } },

    [R_WPASS] = { "WEST PASSAGE", SH_L, 0, 0, 0, 0, C565(160, 120, 76), 0, 0, 0, 0, 0,
        "########"
        "#p.....#"
        "#......#"
        "#...0..#"
        "#......#"
        "#.....p#"
        "########", { IT_COIN, 0 } },

    [R_EPASS] = { "EAST PASSAGE", SH_R, 0, 0, 0, 0, C565(160, 120, 76), 0, 0, 0, 0, 0,
        "########"
        "#.....x#"
        "#......#"
        "#...0..#"
        "#......#"
        "#x.....#"
        "########", { IT_COIN, 0 } },

    [R_DINING] = { "DINING ROOM", SH_STR, 0, 2, 0, 0, C565(190, 80, 80), 0, 4, 0, 0, 0,
        "########"
        "#......#"
        "#.htth.#"
        "#......#"
        "#.h00h.#"
        "#......#"
        "########", { IT_COIN, 0 } },

    [R_KITCHEN] = { "KITCHEN", SH_R, 0, 5, 0, 0, C565(210, 200, 180), 0, 4, 0, 0, 0,
        "########"
        "#s..r..#"
        "#......#"
        "#..0...#"
        "#.t....#"
        "#......#"
        "########", { IT_FOOD, 0 } },

    [R_BEDROOM] = { "BEDROOM", SH_DEAD, 0, 2, 0, 0, C565(170, 60, 66), 0, 6, 0, 0, 0,
        "########"
        "#b..p..#"
        "#......#"
        "#..0...#"
        "#......#"
        "#.x....#"
        "########", { IT_COIN, 0 } },

    [R_CLOSET] = { "CLOSET", SH_DEAD, 0, 0, 0, 0, C565(140, 104, 64), 0, 0, 0, 0, 0,
        "########"
        "#xx..xx#"
        "#r....0#"
        "#......#"
        "#x.....#"
        "#xx..xx#"
        "########", { IT_KEY, 0 } },

    [R_PANTRY] = { "PANTRY", SH_DEAD, 0, 5, 0, 0, C565(220, 208, 186), 0, 4, 0, 0, 0,
        "########"
        "#rr..rr#"
        "#0....1#"
        "#......#"
        "#..xx..#"
        "#r....r#"
        "########", { IT_FOOD, IT_COIN, 0 } },

    [R_TERRACE] = { "TERRACE", SH_L, 0, 4, 4, 0, C565(96, 160, 70), RF_GREEN, 0, 0, 1, 0,
        "########"
        "#p....p#"
        "#..0...#"
        "#......#"
        "#...f..#"
        "#p....p#"
        "########", { IT_GEM, 0 } },

    [R_GARDEN] = { "GARDEN", SH_DEAD, 1, 4, 4, 0, C565(80, 150, 60), RF_GREEN, 0, 0, 1, 0,
        "########"
        "#p....p#"
        "#..0...#"
        "#......#"
        "#...1..#"
        "#p....p#"
        "########", { IT_GEM, IT_COIN, 0 } },

    [R_FOYER] = { "FOYER", SH_T, 1, 6, 0, 1, C565(200, 200, 212), 0, 0, 0, 0, 0,
        "########"
        "#p....p#"
        "#......#"
        "#..R...#"
        "#......#"
        "#.0..1.#"
        "########", { IT_COIN, IT_COIN, 0 } },

    [R_STUDY] = { "STUDY", SH_L, 1, 3, 0, 0, C565(70, 90, 170), 0, 0, 0, 1, 50,
        "########"
        "#BB..BB#"
        "#......#"
        "#.h..0.#"
        "#..t...#"
        "#......#"
        "########", { IT_COIN, 0 } },

    [R_CHAPEL] = { "CHAPEL", SH_STR, 1, 3, 1, 0, C565(90, 105, 190), 0, 0, 0, 0, 0,
        "########"
        "#..a...#"
        "#......#"
        "#.0..1.#"
        "#.h..h.#"
        "#......#"
        "########", { IT_STAR2, IT_COIN, 0 } },

    [R_STORE] = { "STOREROOM", SH_DEAD, 1, 0, 0, 1, C565(150, 112, 68), 0, 0, 0, 0, 0,
        "########"
        "#xx..x.#"
        "#x.0..1#"
        "#......#"
        "#.2..x.#"
        "#x..xx.#"
        "########", { IT_COIN, IT_COIN, IT_STAR, 0 } },

    [R_GEMDEN] = { "DEN OF GEMS", SH_DEAD, 1, 7, 1, 1, C565(110, 220, 200), 0, 0, 0, 0, 0,
        "########"
        "#g....g#"
        "#..0.1.#"
        "#......#"
        "#..2...#"
        "#g....g#"
        "########", { IT_GEM, IT_GEM, IT_GEM, 0 } },

    [R_SECURITY] = { "SECURITY", SH_R, 1, 1, 1, 1, C565(120, 120, 132), 0, 0, 0, 0, 0,
        "########"
        "#x....x#"
        "#..0.1.#"
        "#......#"
        "#..t...#"
        "#x....x#"
        "########", { IT_KEY, IT_KEY, 0 } },

    [R_COMMISSARY] = { "COMMISSARY", SH_STR, 1, 0, 0, 0, C565(220, 170, 90), RF_UNIQUE | RF_SHOP_COM, 0, 0, 0, 0,
        "########"
        "#S....S#"
        "#.KK...#"
        "#......#"
        "#....x.#"
        "#..0...#"
        "########", { IT_COIN, 0 } },

    [R_LOCKSMITH] = { "LOCKSMITH", SH_DEAD, 1, 1, 0, 0, C565(190, 160, 70), RF_UNIQUE | RF_SHOP_LOCK, 0, 0, 0, 0,
        "########"
        "#S....S#"
        "#..KK..#"
        "#......#"
        "#.A..0.#"
        "#......#"
        "########", { IT_KEY, 0 } },

    [R_CONSERV] = { "CONSERVATORY", SH_T, 1, 4, 4, 1, C565(70, 170, 90), RF_GREEN, 0, 0, 2, 0,
        "########"
        "#p.0..p#"
        "#......#"
        "#..f...#"
        "#......#"
        "#p..1.p#"
        "########", { IT_GEM, IT_GEM, 0 } },

    [R_BALLROOM] = { "BALLROOM", SH_T, 2, 6, 0, 1, C565(226, 226, 238), 0, 0, 0, 0, 60,
        "########"
        "#p....p#"
        "#.0..1.#"
        "#..RR..#"
        "#.2..3.#"
        "#p....p#"
        "########", { IT_COIN, IT_COIN, IT_COIN, IT_STAR, 0 } },

    [R_SUITE] = { "MASTER SUITE", SH_DEAD, 2, 2, 0, 1, C565(200, 70, 80), 0, 12, 1, 0, 0,
        "########"
        "#b..c..#"
        "#......#"
        "#..0.1.#"
        "#......#"
        "#..p...#"
        "########", { IT_COIN, IT_STAR, 0 } },

    [R_VAULT] = { "VAULT", SH_DEAD, 2, 7, 1, 2, C565(240, 205, 80), RF_UNIQUE | RF_LOCKED, 0, 0, 0, 0,
        "########"
        "#c0..1c#"
        "#2....3#"
        "#......#"
        "#4....5#"
        "#c....c#"
        "########", { IT_COIN, IT_COIN, IT_COIN, IT_COIN, IT_STAR, IT_COIN } },

    [R_OBSERV] = { "OBSERVATORY", SH_DEAD, 2, 7, 1, 2, C565(120, 150, 240), RF_UNIQUE, 0, 0, 0, 0,
        "########"
        "#....o.#"
        "#......#"
        "#.0....#"
        "#......#"
        "#..x...#"
        "########", { IT_STAR3, 0 } },

    [R_GREATHALL] = { "GREAT HALL", SH_X, 2, 6, 0, 2, C565(240, 240, 250), RF_UNIQUE, 0, 0, 0, 50,
        "########"
        "#p.0..p#"
        "#......#"
        "#....f.#"
        "#......#"
        "#p..1.p#"
        "########", { IT_COIN, IT_COIN, 0 } },
};

/* draftable room ids (everything but the two fixed rooms) */
#define DRAFT_FIRST R_HALLWAY
static const uint8_t k_rarity_weight[3] = { 6, 3, 1 };

#endif
