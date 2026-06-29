#ifndef ROGUE_BAND_H
#define ROGUE_BAND_H
/*
 * ThumbyRogue depth bands. Every ROGUE_BAND_FLOORS floors the dungeon
 * changes theme: floor/wall/pillar blocks, enemy roster weights, and a name
 * shown on entry. After the last authored band the sequence loops — enemy
 * stats keep scaling with depth, so the descent is endlessly escalating.
 */
#include <stdint.h>

#define ROGUE_BAND_FLOORS 4

#define ROGUE_BAND_ROSTER 5

#define ROGUE_BAND_DECO 4

/* Scenery set-pieces: arranged multi-block features stamped into larger
 * rooms (a library wall, a barrel pile, a tomb of coffins, a crystal
 * cluster, a shrine/altar) — NOT single scattered cubes. */
typedef enum {
    FEAT_LIBRARY  = 0,  /* run of bookcases along a wall, 2 tall */
    FEAT_STORE    = 1,  /* barrel + crate pile, some stacked */
    FEAT_TOMB     = 2,  /* grid of sarcophagi */
    FEAT_CRYSTALS = 3,  /* crystal-cube cluster ringed by shard sprites */
    FEAT_ALTAR    = 4,  /* hand-authored shrine: stepped dais + braziers */
} RogueFeatKind;

/* Prop bitmask: which furniture props this band scatters in its rooms. */
#define ROGUE_PROP_TABLE   (1u << 0)
#define ROGUE_PROP_BRAZIER (1u << 1)

typedef struct {
    const char *name;
    uint8_t floor, wall, pillar;        /* block ids for the reskin */
    uint8_t bg_top, bg_sub;             /* surrounding terrain surface + subsurface */
    uint8_t roster[ROGUE_BAND_ROSTER];  /* EnemyType ids this band spawns */
    uint8_t roster_n;
    uint16_t tint;                      /* banner colour */
    uint8_t feats[ROGUE_BAND_DECO];     /* RogueFeatKind set-pieces this band uses */
    uint8_t feats_n;
    uint8_t sprites[ROGUE_BAND_DECO];   /* cross-sprite scenery scattered (BLK_*) */
    uint8_t sprites_n;
    uint8_t prop_mask;                  /* ROGUE_PROP_* furniture this band uses */
} RogueBand;

/* Band for a given depth (1-based). Loops after the last authored band. */
const RogueBand *rogue_band_get(int depth);
int  rogue_band_count(void);
/* True on the last floor of a band (boss/champion floor). */
int  rogue_band_is_boss_floor(int depth);

#endif /* ROGUE_BAND_H */
