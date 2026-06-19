/* Meshes for the materials demo: a flat floor quad in the X-Z plane (y=0). */
#ifndef MAT_MESHES_H
#define MAT_MESHES_H

#include "mote_mesh.h"
#include "mote_config.h"

/* Floor: a unit quad in the X-Z plane (y=0), normal up. Scaled at draw time. */
static const MeshVert k_floor_v[4] = {
    {-127, 0, -127}, {127, 0, -127}, {127, 0, 127}, {-127, 0, 127},
};
#define MAT_FLOOR MOTE_RGB565(54, 60, 78)
static const MeshFace k_floor_f[2] = {
    {0, 3, 2,  0, 127, 0, MAT_FLOOR}, {0, 2, 1,  0, 127, 0, MAT_FLOOR},
};
static const Mesh k_floor_mesh = { k_floor_v, k_floor_f, 4, 2, 1.0f, 1.5f, 0 };

#endif
