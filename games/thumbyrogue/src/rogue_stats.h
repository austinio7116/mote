#ifndef ROGUE_STATS_H
#define ROGUE_STATS_H
/*
 * Aggregated hero stats, recomputed from the 6 equipped items (their base
 * armor/damage + affixes + socketed gems + which aspects are active).
 */
#include <stdint.h>
#include "rogue_items.h"

typedef struct {
    int max_life;    /* 100 + life affixes + ruby gems */
    int armor;       /* base armor + armor affixes + topaz gems */
    int flat_dmg;    /* + weapon flat damage affixes */
    int dmg_pct;     /* +% damage */
    int crit;        /* crit chance %  (base 5) */
    int crit_dmg;    /* crit damage %  (base 150) */
    int atk_spd;     /* +% attack speed */
    int move_spd;    /* +% move speed */
    int life_on_hit;
    int resist;      /* +% damage resist (sapphire + affixes) */
    uint32_t aspects;/* bitmask of (1<<AspectId) from equipped legendaries */
    uint8_t elem;    /* ElementId — the weapon's element (affix or weapon gem) */
    int16_t elem_pow;/* element magnitude (burn dmg / chill / poison total) */
} RogueStats;

void  rogue_stats_compute(RogueStats *out, const RogueItem equip[SLOT_COUNT]);
float rogue_stats_reduction(int armor);   /* armor -> 0..0.8 damage reduction */

#endif /* ROGUE_STATS_H */
