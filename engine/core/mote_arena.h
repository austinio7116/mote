/*
 * Mote — load-time arena allocator.
 *
 * The engine no longer carries worst-case static pools. Instead the OS hands a
 * single SRAM block to mote_arena_init() at game load; every resizable engine
 * buffer (draw list, depth, physics pools, splats...) and the game's own large
 * buffers are bump-allocated from it, sized by the game's MoteConfig. On game
 * exit the arena is reset and the next game gets the whole block again.
 *
 * Rules: bump-only (no per-allocation free), 8-byte aligned, deterministic, no
 * fragmentation. Over-allocation returns NULL — the loader treats that as
 * "this game's declared pools don't fit" and refuses to launch (clear failure,
 * never silent corruption).
 */
#ifndef MOTE_ARENA_H
#define MOTE_ARENA_H

#include <stdint.h>
#include <stddef.h>

typedef struct MoteArena {
    uint8_t *base;
    size_t   size;
    size_t   used;
    int      overflow;   /* set once any alloc didn't fit */
} MoteArena;

void  mote_arena_init(MoteArena *a, void *base, size_t size);
void *mote_arena_alloc(MoteArena *a, size_t n);   /* 8-byte aligned, zeroed; NULL on overflow */
void  mote_arena_reset(MoteArena *a);             /* reclaim everything (between games) */

static inline size_t mote_arena_used(const MoteArena *a) { return a->used; }
static inline size_t mote_arena_free(const MoteArena *a) { return a->size - a->used; }

#endif /* MOTE_ARENA_H */
