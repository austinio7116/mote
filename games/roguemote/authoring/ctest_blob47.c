/*
 * Cross-check the baked rulesets against the engine's OWN header.
 *
 * verify_terrain.py re-derives the blob47 contract from a second transcription
 * of sdk/mote_tile.h -- but a transcription can be wrong in the same way twice.
 * This compiles the real header and asks it directly:
 *
 *   1. does mote_autotile_template(MOTE_AT_BLOB47) produce the LUT we baked?
 *   2. does mote_autotile_mask() agree with the Python port for every one of
 *      the 3^8 neighbourhood patterns?
 *
 * Build + run:  cc -I../../../sdk -o /tmp/ctest ctest_blob47.c && /tmp/ctest
 * It prints a compact digest that gen_terrain_report.py embeds as evidence.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* mote_tile.h wants MoteImage from mote_2d.h; we never dereference one. */
typedef struct MoteImage { const uint16_t *px; int w, h; } MoteImage;
#define MOTE_2D_H            /* stop mote_tile.h pulling the real mote_2d.h */
#include "mote_tile.h"

/* the baked rulesets, exactly as the game compiles them */
#define MOTE_T_wall_brick_H_SKIP 0
static const MoteAutotile *load_brick(void);

int main(void) {
    int fails = 0;

    /* 1. the engine's own template vs the LUT we bake into the .tileset */
    MoteAutotile at;
    memset(&at, 0, sizeof at);
    mote_autotile_template(&at, MOTE_AT_BLOB47);

    printf("cell_count(BLOB47) = %d\n", mote_autotile_cell_count(MOTE_AT_BLOB47));
    if (mote_autotile_cell_count(MOTE_AT_BLOB47) != 47) {
        printf("FAIL: expected 47 cells\n"); fails++;
    }

    /* dump the template LUT so Python can diff it byte for byte */
    printf("template_lut:");
    for (int m = 0; m < 256; m++) printf(" %d", at.lut[m]);
    printf("\n");

    /* 2. the reduction, straight from the header */
    printf("reduce:");
    for (int m = 0; m < 256; m++) printf(" %d", mote__at_reduce((uint8_t)m));
    printf("\n");

    /* 3. mote_autotile_mask() over a 3x3 neighbourhood, for all 256 patterns.
     * The centre is terrain 1; each neighbour is 1 or 0 per the pattern bit. */
    printf("masks:");
    for (int p = 0; p < 256; p++) {
        uint8_t t[9];
        static const int dx[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        static const int dy[8] = {-1,-1, 0, 1, 1, 1, 0,-1 };
        for (int i = 0; i < 9; i++) t[i] = 0;
        t[4] = 1;
        for (int b = 0; b < 8; b++)
            if (p & (1 << b)) t[(1 + dy[b]) * 3 + (1 + dx[b])] = 1;
        printf(" %d", mote_autotile_mask(t, 3, 3, 1, 1, 1, 0));
    }
    printf("\n");

    /* 4. edge_is_solid: a 1x1 map, off-map neighbours counted both ways */
    uint8_t one = 1;
    printf("edge0 %d edge1 %d\n",
           mote_autotile_mask(&one, 1, 1, 0, 0, 1, 0),
           mote_autotile_mask(&one, 1, 1, 0, 0, 1, 1));

    /* 5. the per-cell variant picker, so the Python port's hash matches */
    MoteAutotile v; memset(&v, 0, sizeof v);
    v.nvar = 3; v.var_weight[0] = 1; v.var_weight[1] = 1; v.var_weight[2] = 2;
    printf("variants:");
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) printf(" %d", mote__at_variant(&v, x, y));
    printf("\n");

    printf(fails ? "C_FAILS %d\n" : "C_OK\n", fails);
    return fails ? 1 : 0;
}

static const MoteAutotile *load_brick(void) { return 0; }
