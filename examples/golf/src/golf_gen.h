/*
 * golf_gen — single golf-hole generator. The TERRAIN heightmap (multi-octave,
 * style-aware value noise) is VENDORED from ThumbyGolf (src/golf_terrain.c:
 * hash32 / noise2d / noise2d_aniso + the per-style amplitude table). The hole
 * LAYOUT (tee -> dogleg -> cup, fairway corridor, green pad, lie regions, tree
 * clustering) is a Mote adaptation of ThumbyGolf's route/carve layers.
 * Units: metres. 1 cell = 1 m, like ThumbyGolf.
 */
#ifndef GOLF_GEN_H
#define GOLF_GEN_H
#include <stdint.h>

enum { GOLF_ROUGH=0, GOLF_FAIRWAY=1, GOLF_GREEN=2, GOLF_TEE=3, GOLF_BUNKER=4, GOLF_WATER=5 };
enum { GOLF_LINKS=0, GOLF_PARKLAND=1, GOLF_HEATHLAND=2 };

typedef struct {
    uint32_t seed; int style; int par;
    float tee_x, tee_z, cup_x, cup_z, bend_x, bend_z;
    float tee_h, cup_h, length_m;
    float min_x, max_x, min_z, max_z;     /* bounding box (m) */
    float water_level;                    /* land below this floods */
    int   n_bunker; float bunker_x[4], bunker_z[4], bunker_r[4];
} GolfHole;

void  golf_generate(GolfHole *h, uint32_t seed);
float golf_surface(const GolfHole *h, float x, float z);  /* land surface, no water */
float golf_height(const GolfHole *h, float x, float z);   /* surface, clamped up to water */
int   golf_lie(const GolfHole *h, float x, float z);      /* GOLF_* lie */
float golf_route_dist(const GolfHole *h, float x, float z);
int   golf_tree(const GolfHole *h, float x, float z);     /* 1 if a tree sits here */

#endif
