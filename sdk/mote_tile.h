#ifndef MOTE_TILE_H
#define MOTE_TILE_H
/*
 * Mote autotile (rule-tile) system.
 *
 * You store a LOGICAL terrain map — one byte per cell, 0 = empty, 1..N = a
 * terrain id. The engine renders it directly (see scene2d_set_autotiles in
 * mote_api.h): for each visible cell it looks at the 8 neighbours, builds a
 * "same terrain" bitmask, and picks the atlas cell from the ruleset's LUT.
 * There is NO resolved-tilemap buffer — the only storage is the terrain map you
 * already keep, so autotiling is free in RAM and works for maps that change at
 * runtime (procgen caves, destructible terrain).
 *
 * A MoteAutotile is one terrain's ruleset: its tileset + a 256-entry LUT mapping
 * the neighbour mask to an atlas cell. Build the LUT from a template with
 * mote_autotile_template(), or bake a custom one in Mote Studio's Tiles tab.
 */
#include <stdint.h>
#include "mote_2d.h"

/* 8-neighbour bitmask — one bit per neighbour (clockwise from North). */
enum {
    MOTE_NB_N  = 1u << 0, MOTE_NB_NE = 1u << 1, MOTE_NB_E = 1u << 2, MOTE_NB_SE = 1u << 3,
    MOTE_NB_S  = 1u << 4, MOTE_NB_SW = 1u << 5, MOTE_NB_W = 1u << 6, MOTE_NB_NW = 1u << 7
};

/* Template rulesets (canonical sheet layouts). */
enum {
    MOTE_AT_BLOB47   = 0,   /* 8-bit corner-aware terrain — caves, water, cliffs (Spelunky/Zelda/golf) */
    MOTE_AT_EDGE16   = 1,   /* 4-cardinal blocky platforms/pipes (Mario), a 4x4 sheet */
    MOTE_AT_NINESLICE= 2,   /* 3x3 nine-slice for rectangular regions (platforms, UI panels) */
    MOTE_AT_WANG16   = 3    /* 2-corner Wang, a 4x4 sheet */
};

typedef struct MoteAutotile {
    const MoteImage *sheet;     /* the tileset atlas (ncell wide x nvar tall) */
    uint16_t tile_w, tile_h;    /* cell size in the atlas */
    uint8_t  lut[256];          /* neighbour-mask -> atlas cell index (row 0) */
    uint8_t  edge_is_solid;     /* 1 = off-map neighbours count as the same terrain (seamless edges) */
    uint8_t  nvar;              /* random variants per config (atlas rows); 0/1 = none. The engine
                                 * picks a row per cell from its position, so big areas don't repeat. */
} MoteAutotile;

/* deterministic per-cell hash, for picking a random variant by position. */
static inline unsigned mote__at_hash(int x, int y) {
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u;
    h ^= h >> 13; return h * 1274126177u;
}

/* Drop a corner bit unless BOTH its adjacent cardinals are set — the reduction
 * that collapses 256 neighbour configurations to the 47 distinct blob tiles. */
static inline uint8_t mote__at_reduce(uint8_t m) {
    uint8_t r = (uint8_t)(m & (MOTE_NB_N | MOTE_NB_E | MOTE_NB_S | MOTE_NB_W));
    if ((m & MOTE_NB_NE) && (r & MOTE_NB_N) && (r & MOTE_NB_E)) r |= MOTE_NB_NE;
    if ((m & MOTE_NB_SE) && (r & MOTE_NB_S) && (r & MOTE_NB_E)) r |= MOTE_NB_SE;
    if ((m & MOTE_NB_SW) && (r & MOTE_NB_S) && (r & MOTE_NB_W)) r |= MOTE_NB_SW;
    if ((m & MOTE_NB_NW) && (r & MOTE_NB_N) && (r & MOTE_NB_W)) r |= MOTE_NB_NW;
    return r;
}

/* Number of distinct cells a template's sheet needs (47/16/9/16). */
static inline int mote_autotile_cell_count(int kind) {
    if (kind == MOTE_AT_NINESLICE) return 9;
    if (kind == MOTE_AT_EDGE16 || kind == MOTE_AT_WANG16) return 16;
    /* BLOB47: count distinct reduced masks. */
    int seen[256]; for (int i = 0; i < 256; i++) seen[i] = 0;
    int n = 0; for (int m = 0; m < 256; m++) { uint8_t r = mote__at_reduce((uint8_t)m); if (!seen[r]) { seen[r] = 1; n++; } }
    return n;
}

/* Fill at->lut from a template. Set sheet / tile_w / tile_h / edge_is_solid yourself. */
static inline void mote_autotile_template(MoteAutotile *at, int kind) {
    int m;
    if (kind == MOTE_AT_EDGE16) {
        for (m = 0; m < 256; m++) { uint8_t c = 0;
            if (m & MOTE_NB_N) c |= 1; if (m & MOTE_NB_E) c |= 2;
            if (m & MOTE_NB_S) c |= 4; if (m & MOTE_NB_W) c |= 8;
            at->lut[m] = c;                                   /* 0..15 in a 4x4 sheet */
        }
    } else if (kind == MOTE_AT_NINESLICE) {
        for (m = 0; m < 256; m++) {
            int W = !!(m & MOTE_NB_W), E = !!(m & MOTE_NB_E), N = !!(m & MOTE_NB_N), S = !!(m & MOTE_NB_S);
            int col = (!W && E) ? 0 : (W && !E) ? 2 : 1;      /* left / right / middle */
            int row = (!N && S) ? 0 : (N && !S) ? 2 : 1;      /* top  / bottom / middle */
            at->lut[m] = (uint8_t)(row * 3 + col);            /* 3x3 sheet */
        }
    } else if (kind == MOTE_AT_WANG16) {
        for (m = 0; m < 256; m++) { uint8_t c = 0;           /* a corner is set when its 3 cells agree */
            if ((m & MOTE_NB_N) && (m & MOTE_NB_W) && (m & MOTE_NB_NW)) c |= 1;   /* TL */
            if ((m & MOTE_NB_N) && (m & MOTE_NB_E) && (m & MOTE_NB_NE)) c |= 2;   /* TR */
            if ((m & MOTE_NB_S) && (m & MOTE_NB_W) && (m & MOTE_NB_SW)) c |= 4;   /* BL */
            if ((m & MOTE_NB_S) && (m & MOTE_NB_E) && (m & MOTE_NB_SE)) c |= 8;   /* BR */
            at->lut[m] = c;                                   /* 0..15 in a 4x4 sheet */
        }
    } else { /* MOTE_AT_BLOB47 — canonical order: reduced masks, first-seen ascending */
        int idx[256]; for (m = 0; m < 256; m++) idx[m] = -1;
        int n = 0;
        for (m = 0; m < 256; m++) { uint8_t r = mote__at_reduce((uint8_t)m); if (idx[r] < 0) idx[r] = n++; }
        for (m = 0; m < 256; m++) at->lut[m] = (uint8_t)idx[mote__at_reduce((uint8_t)m)];
    }
}

/* The 8-neighbour "same terrain" mask for one cell (shared by the engine raster). */
static inline int mote_autotile_mask(const uint8_t *terrain, int cols, int rows,
                                     int c, int r, uint8_t match, int edge_is_solid) {
    int m = 0;
    #define MOTE__NB(dx, dy, bit) do { int cc = c + (dx), rr = r + (dy); int same; \
        if (cc < 0 || cc >= cols || rr < 0 || rr >= rows) same = edge_is_solid;    \
        else same = (terrain[rr * cols + cc] == match);                            \
        if (same) m |= (bit); } while (0)
    MOTE__NB( 0, -1, MOTE_NB_N);  MOTE__NB( 1, -1, MOTE_NB_NE); MOTE__NB( 1, 0, MOTE_NB_E);
    MOTE__NB( 1,  1, MOTE_NB_SE); MOTE__NB( 0,  1, MOTE_NB_S);  MOTE__NB(-1, 1, MOTE_NB_SW);
    MOTE__NB(-1,  0, MOTE_NB_W);  MOTE__NB(-1, -1, MOTE_NB_NW);
    #undef MOTE__NB
    return m;
}

/* The 8-neighbour mask for a LAYERED level map, where map[cell] is a bitmask of
 * which layers occupy that cell (bit L). A neighbour counts for layer `bit` if
 * its bit is set (so each layer autotiles independently, even where layers
 * overlap). Off-map neighbours use edge_is_solid. */
static inline int mote_autotile_mask_layer(const uint8_t *map, int cols, int rows,
                                           int c, int r, int bit, int edge_is_solid) {
    int m = 0;
    #define MOTE__LB(dx, dy, b) do { int cc = c + (dx), rr = r + (dy); int same;       \
        if (cc < 0 || cc >= cols || rr < 0 || rr >= rows) same = edge_is_solid;         \
        else same = (int)((map[rr * cols + cc] >> bit) & 1u);                           \
        if (same) m |= (b); } while (0)
    MOTE__LB( 0, -1, MOTE_NB_N);  MOTE__LB( 1, -1, MOTE_NB_NE); MOTE__LB( 1, 0, MOTE_NB_E);
    MOTE__LB( 1,  1, MOTE_NB_SE); MOTE__LB( 0,  1, MOTE_NB_S);  MOTE__LB(-1, 1, MOTE_NB_SW);
    MOTE__LB(-1,  0, MOTE_NB_W);  MOTE__LB(-1, -1, MOTE_NB_NW);
    #undef MOTE__LB
    return m;
}

/* Optional: pre-resolve a whole logical map into concrete atlas indices (a
 * MoteTilemap `cells[]`), for games that prefer a static baked tilemap over the
 * engine's render-time path. Cells not equal to `match` are left untouched. */
static inline void mote_autotile_resolve(const MoteAutotile *at, const uint8_t *terrain,
                                         uint8_t *out, int cols, int rows, uint8_t match) {
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            if (terrain[i] != match) continue;
            out[i] = at->lut[mote_autotile_mask(terrain, cols, rows, c, r, match, at->edge_is_solid)];
        }
}

#endif /* MOTE_TILE_H */
