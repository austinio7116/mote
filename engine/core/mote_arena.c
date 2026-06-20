#include "mote_arena.h"
#include <string.h>

void mote_arena_init(MoteArena *a, void *base, size_t size) {
    a->base = (uint8_t *)base;
    a->size = size;
    a->used = 0;
    a->overflow = 0;
}

void *mote_arena_alloc(MoteArena *a, size_t n) {
    size_t off = (a->used + 7u) & ~(size_t)7u;   /* 8-byte align */
    if (n == 0 || off + n > a->size) { a->overflow = 1; return NULL; }
    void *p = a->base + off;
    a->used = off + n;
    memset(p, 0, n);                              /* BSS semantics */
    return p;
}

void mote_arena_reset(MoteArena *a) {
    a->used = 0;
    a->overflow = 0;
}
