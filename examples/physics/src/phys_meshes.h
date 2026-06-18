/* Meshes for the physics demo: a multicolour cube (a "body") and a floor quad. */
#ifndef PHYS_MESHES_H
#define PHYS_MESHES_H

#include "mote_mesh.h"
#include "mote_config.h"

static const MeshVert k_cube_v[8] = {
    {-127,-127,-127},{127,-127,-127},{127,127,-127},{-127,127,-127},
    {-127,-127, 127},{127,-127, 127},{127,127, 127},{-127,127, 127},
};
#define R MOTE_RGB565(230, 90, 90)
#define G MOTE_RGB565(90, 210, 110)
#define B MOTE_RGB565(90, 150, 240)
static const MeshFace k_cube_f[12] = {
    {4,5,6,0,0,127,B},{4,6,7,0,0,127,B}, {1,0,3,0,0,-127,B},{1,3,2,0,0,-127,B},
    {5,1,2,127,0,0,R},{5,2,6,127,0,0,R}, {0,4,7,-127,0,0,R},{0,7,3,-127,0,0,R},
    {7,6,2,0,127,0,G},{7,2,3,0,127,0,G}, {0,1,5,0,-127,0,G},{0,5,4,0,-127,0,G},
};
static const Mesh k_body_mesh = { k_cube_v, k_cube_f, 8, 12, 1.0f, 1.8f, 0 };

/* Floor: a unit quad in the X-Z plane (y=0), normal up. */
static const MeshVert k_floor_v[4] = {
    {-127, 0, -127}, {127, 0, -127}, {127, 0, 127}, {-127, 0, 127},
};
#define F MOTE_RGB565(60, 70, 95)
static const MeshFace k_floor_f[2] = {
    {0, 3, 2,  0, 127, 0, F}, {0, 2, 1,  0, 127, 0, F},
};
static const Mesh k_floor_mesh = { k_floor_v, k_floor_f, 4, 2, 1.0f, 1.5f, 0 };

#endif
