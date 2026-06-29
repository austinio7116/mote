#include "rogue_band.h"
#include "rogue_enemy.h"
#include "craft_blocks.h"

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

/* Authored bands; the sequence loops after the last (depth keeps scaling).
 * Each band has a distinct identity and includes a ranged threat (archer /
 * fire sprite) plus, where it fits, a flyer (bat). Toughness escalates with
 * depth, and across the five bands every one of the 11 enemy types appears. */
static const RogueBand BANDS[] = {
    { "THE CRYPT",   BLK_PLANK,    BLK_COBBLE,    BLK_STONE,          /* vermin & risen dead — warm wood floor vs grey cobble */
      BLK_GRASS,    BLK_DIRT,        /* graveyard grass surround */
      { EN_RAT, EN_BAT, EN_SKELETON, EN_ARCHER }, 4, RGB(180,180,190),
      { FEAT_LIBRARY, FEAT_TOMB, FEAT_STORE, FEAT_ALTAR }, 4,
      { BLK_BONES, BLK_COBWEB, BLK_RUBBLE }, 3,
      ROGUE_PROP_TABLE | ROGUE_PROP_BRAZIER },
    { "THE CAVERNS", BLK_DIRT,     BLK_CAVE_ROCK, BLK_STONE,          /* earthy cave — warm tan rock, dirt ground */
      BLK_DIRT,     BLK_CAVE_ROCK,   /* earthen mounds */
      { EN_SPIDER, EN_KOBOLD, EN_GOBLIN, EN_BAT, EN_SLIME }, 5, RGB(168,138,96),
      { FEAT_CRYSTALS, FEAT_STORE }, 2,
      { BLK_RUBBLE, BLK_SHARDS, BLK_BONES }, 3,
      ROGUE_PROP_BRAZIER },
    { "FUNGAL DEEP", BLK_MYCELIUM, BLK_FUNGAL_WALL, BLK_MUSHROOM,     /* bioluminescent spore caverns */
      BLK_MYCELIUM, BLK_FUNGAL_WALL, /* spore overgrowth surround */
      { EN_SLIME, EN_ZOMBIE, EN_FIRESPRITE, EN_SPIDER, EN_KOBOLD }, 5, RGB(150,90,210),
      { FEAT_CRYSTALS }, 1,
      { BLK_FUNGI, BLK_SHARDS, BLK_BONES }, 3,
      0 },
    { "FROSTVAULT",  BLK_RFLOOR,   BLK_ICE,       BLK_SNOWY_ROCK,     /* frozen undead — flagstone vault floor under icy walls */
      BLK_SNOW,     BLK_SNOWY_ROCK,  /* snowfield surround */
      { EN_SKELETON, EN_ARCHER, EN_ZOMBIE, EN_BAT }, 4, RGB(170,210,240),
      { FEAT_TOMB, FEAT_CRYSTALS, FEAT_STORE }, 3,
      { BLK_RUBBLE, BLK_SHARDS, BLK_BONES }, 3,
      ROGUE_PROP_BRAZIER },
    { "THE INFERNO", BLK_OBSIDIAN, BLK_COBBLE,    BLK_REDSTONE_BLOCK, /* hell's legions */
      BLK_OBSIDIAN, BLK_STONE,       /* charred obsidian surround */
      { EN_DEMON, EN_FIRESPRITE, EN_GOBLIN, EN_ARCHER, EN_KOBOLD }, 5, RGB(240,120,60),
      { FEAT_STORE, FEAT_CRYSTALS, FEAT_ALTAR }, 3,
      { BLK_RUBBLE, BLK_BONES }, 2,
      ROGUE_PROP_BRAZIER },
};
#define N_BANDS ((int)(sizeof(BANDS)/sizeof(BANDS[0])))

int rogue_band_count(void) { return N_BANDS; }

const RogueBand *rogue_band_get(int depth) {
    if (depth < 1) depth = 1;
    int b = (depth - 1) / ROGUE_BAND_FLOORS;
    return &BANDS[b % N_BANDS];
}

int rogue_band_is_boss_floor(int depth) {
    return (depth % ROGUE_BAND_FLOORS) == 0;
}
