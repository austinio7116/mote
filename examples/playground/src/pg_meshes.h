/*
 * Meshes for the physics playground: a big flat ground quad, a brown ramp
 * slab (a flat box), and a low wall slab. Vertices are quantised int8 in
 * [-127,127]; world half-extent = mesh.scale meters along each axis (the
 * mesh is scaled uniformly, so the int8 proportions ARE the box shape).
 */
#ifndef PG_MESHES_H
#define PG_MESHES_H

#include "mote_mesh.h"
#include "mote_config.h"

/* ---- Ground: a large quad in the X-Z plane (y=0), normal up. ---------- */
static const MeshVert k_ground_v[4] = {
    {-127, 0, -127}, {127, 0, -127}, {127, 0, 127}, {-127, 0, 127},
};
#define GR MOTE_RGB565(64, 92, 60)   /* mossy green-grey */
static const MeshFace k_ground_f[2] = {
    {0, 3, 2,  0, 127, 0, GR}, {0, 2, 1,  0, 127, 0, GR},
};
/* scale 6.0 -> ground spans +-6 m (12 x 12 m play area). */
static const Mesh k_ground_mesh = { k_ground_v, k_ground_f, 4, 2, 6.0f, 9.0f, 0 };

/* ---- Ramp slab: a wide, thin brown box. Local extents are proportional
 *      to the int8 coords; we scale uniformly, so to get a slab of world
 *      half-extents (hx,hy,hz) the verts encode the SAME proportions and we
 *      pick scale = max(hx,hy,hz) with the others scaled down accordingly.
 *      Here the slab is 2.0 x 0.20 x 1.4 (half-extents) -> proportions
 *      127 : 12.7 : 88.9 of a scale=2.0 cube. */
#define SX 127   /* hx = 2.0  */
#define SY 13    /* hy = 0.205 */
#define SZ 89    /* hz = 1.40 */
static const MeshVert k_slab_v[8] = {
    {-SX,-SY,-SZ},{SX,-SY,-SZ},{SX,SY,-SZ},{-SX,SY,-SZ},
    {-SX,-SY, SZ},{SX,-SY, SZ},{SX,SY, SZ},{-SX,SY, SZ},
};
#define BR  MOTE_RGB565(150, 96, 52)   /* brown top/sides */
#define BRD MOTE_RGB565(110, 70, 38)   /* darker brown */
static const MeshFace k_slab_f[12] = {
    {4,5,6,0,0,127,BRD},{4,6,7,0,0,127,BRD}, {1,0,3,0,0,-127,BRD},{1,3,2,0,0,-127,BRD},
    {5,1,2,127,0,0,BR}, {5,2,6,127,0,0,BR},  {0,4,7,-127,0,0,BR}, {0,7,3,-127,0,0,BR},
    {7,6,2,0,127,0,BR}, {7,2,3,0,127,0,BR},  {0,1,5,0,-127,0,BRD},{0,5,4,0,-127,0,BRD},
};
static const Mesh k_slab_mesh = { k_slab_v, k_slab_f, 8, 12, 2.0f, 2.5f, 0 };
#undef SX
#undef SY
#undef SZ

/* ---- Wall slab: a low, narrow grey box. Half-extents 2.4 x 0.4 x 0.18
 *      -> proportions 127 : 21 : 9.5 of a scale=2.4 cube. */
#define WX 127
#define WY 21
#define WZ 10
static const MeshVert k_wall_v[8] = {
    {-WX,-WY,-WZ},{WX,-WY,-WZ},{WX,WY,-WZ},{-WX,WY,-WZ},
    {-WX,-WY, WZ},{WX,-WY, WZ},{WX,WY, WZ},{-WX,WY, WZ},
};
#define WL  MOTE_RGB565(120, 124, 132)
#define WLD MOTE_RGB565(86, 90, 100)
static const MeshFace k_wall_f[12] = {
    {4,5,6,0,0,127,WLD},{4,6,7,0,0,127,WLD}, {1,0,3,0,0,-127,WLD},{1,3,2,0,0,-127,WLD},
    {5,1,2,127,0,0,WL}, {5,2,6,127,0,0,WL},  {0,4,7,-127,0,0,WL}, {0,7,3,-127,0,0,WL},
    {7,6,2,0,127,0,WL}, {7,2,3,0,127,0,WL},  {0,1,5,0,-127,0,WLD},{0,5,4,0,-127,0,WLD},
};
static const Mesh k_wall_mesh = { k_wall_v, k_wall_f, 8, 12, 2.4f, 2.5f, 0 };
#undef WX
#undef WY
#undef WZ

#endif
