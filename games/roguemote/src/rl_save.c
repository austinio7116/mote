/*
 * Save/restore and the high-score table.
 *
 * Only the Player struct and the two seeds are persisted. The level is NOT --
 * regenerating it from the depth is what a Moria-style game already does when
 * you take the stairs, so a reload landing you on a fresh floor of the same
 * depth is consistent with the rest of the game rather than a compromise.
 *
 * The blob carries a magic + version word so a save written by an older layout
 * is rejected instead of being read as garbage stats. Bump it whenever Player
 * changes -- v4 added the light slot, v5 the blessing timer.
 */
#include "rl.h"

#define SAVE_KEY   "roguemote.sav"
#define SCORE_KEY  "roguemote.hi"
#define SAVE_MAGIC 0x524D5306u          /* 'RMS' + layout version 6 */

typedef struct {
    uint32_t magic;
    uint32_t seed, world_seed, turn;
    Player   pl;
} SaveBlob;

typedef struct {
    uint32_t magic;
    int32_t  score;
    uint8_t  cls, depth, level, pad;
} ScoreBlob;

void rl_save(void) {
    SaveBlob b;
    b.magic = SAVE_MAGIC;
    b.seed = g_seed; b.world_seed = g_world_seed; b.turn = g_turn;
    b.pl = g_pl;
    g_api->kv_save(SAVE_KEY, &b, (int)sizeof b);
}

int rl_load(void) {
    SaveBlob b;
    int n = g_api->kv_load(SAVE_KEY, &b, (int)sizeof b);
    if (n != (int)sizeof b || b.magic != SAVE_MAGIC) return 0;
    g_seed = b.seed ? b.seed : 1;
    g_world_seed = b.world_seed ? b.world_seed : 1;
    g_turn = b.turn;
    g_pl = b.pl;

    /* Flavours are shuffled from the RNG, so they must be re-derived from the
     * SAME seed the character was rolled with -- otherwise a reload renames
     * every unidentified potion in the pack. */
    uint32_t keep = g_seed;
    g_seed = b.world_seed ? b.world_seed : 1;
    rl_item_init_flavours();
    g_seed = keep;

    if (g_pl.depth == 0) rl_gen_overworld();
    else                 rl_gen_level(g_pl.depth);
    return 1;
}

void rl_wipe_save(void) { g_api->kv_save(SAVE_KEY, "", 0); }

/* Score is the classic Angband shape: experience, plus a bonus for how deep
 * you got, so a shallow grinder never outranks a deep diver. */
static int32_t score_of(void) {
    return g_pl.xp + (int32_t)g_pl.deepest * 250 + (int32_t)g_pl.bosses_slain * 1500;
}

void rl_score_submit(void) {
    ScoreBlob cur, prev;
    cur.magic = SAVE_MAGIC;
    cur.score = score_of();
    cur.cls = g_pl.cls; cur.depth = g_pl.deepest; cur.level = g_pl.level; cur.pad = 0;
    int n = g_api->kv_load(SCORE_KEY, &prev, (int)sizeof prev);
    if (n == (int)sizeof prev && prev.magic == SAVE_MAGIC && prev.score >= cur.score) return;
    g_api->kv_save(SCORE_KEY, &cur, (int)sizeof cur);
}

int rl_score_best(void) {
    ScoreBlob b;
    int n = g_api->kv_load(SCORE_KEY, &b, (int)sizeof b);
    if (n != (int)sizeof b || b.magic != SAVE_MAGIC) return 0;
    return (int)b.score;
}
