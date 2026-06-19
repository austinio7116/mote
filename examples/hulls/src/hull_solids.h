/* Clean convex solids: a unit render mesh + a matching MoteHull collider
 * (faces + edges, so the SAT/clip manifold gives stable multi-point contacts).
 * Place a body as shape=MOTE_SHAPE_HULL, shape_data=&<x>_hull, and render
 * <x>_rmesh scaled by the solid's half-size. Verified stacking/tumbling stable. */
#ifndef HULL_SOLIDS_H
#define HULL_SOLIDS_H
#include "mote_api.h"

/* ===== CUBE / die (half-extent 0.30) ===== */
static const MeshVert cube_rv[8] = {
    {-127,-127,-127},{127,-127,-127},{127,127,-127},{-127,127,-127},
    {-127,-127, 127},{127,-127, 127},{127,127, 127},{-127,127, 127},
};
static const MeshFace cube_rf[12] = {
    {4,5,6, 0,0,127,  MOTE_RGB565(238,236,226)},{4,6,7, 0,0,127,  MOTE_RGB565(238,236,226)},
    {1,0,3, 0,0,-127, MOTE_RGB565(206,204,196)},{1,3,2, 0,0,-127, MOTE_RGB565(206,204,196)},
    {5,1,2, 127,0,0,  MOTE_RGB565(224,222,212)},{5,2,6, 127,0,0,  MOTE_RGB565(224,222,212)},
    {0,4,7,-127,0,0,  MOTE_RGB565(196,194,186)},{0,7,3,-127,0,0,  MOTE_RGB565(196,194,186)},
    {7,6,2, 0,127,0,  MOTE_RGB565(246,244,234)},{7,2,3, 0,127,0,  MOTE_RGB565(246,244,234)},
    {0,1,5, 0,-127,0, MOTE_RGB565(184,182,174)},{0,5,4, 0,-127,0, MOTE_RGB565(184,182,174)},
};
static const Mesh cube_rmesh = { cube_rv, cube_rf, 8, 12, 1.0f, 1.74f, 0 };

static const Vec3 cube_hv[8] = {
    {-0.3f,-0.3f,-0.3f},{-0.3f,-0.3f,0.3f},{-0.3f,0.3f,-0.3f},{-0.3f,0.3f,0.3f},
    { 0.3f,-0.3f,-0.3f},{ 0.3f,-0.3f,0.3f},{ 0.3f,0.3f,-0.3f},{ 0.3f,0.3f,0.3f},
};
static const Vec3   cube_hfn[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static const uint8_t cube_hfo[7] = {0,4,8,12,16,20,24};
static const uint8_t cube_hfv[24] = {4,5,7,6, 0,2,3,1, 2,6,7,3, 0,1,5,4, 1,3,7,5, 0,4,6,2};
static const uint8_t cube_hev[24] = {0,1,1,3,3,2,2,0, 4,5,5,7,7,6,6,4, 0,4,1,5,3,7,2,6};
static const MoteHull cube_hull = { cube_hv,8, cube_hfn,cube_hfo,cube_hfv,6, cube_hev,12, 0.52f };
#define CUBE_S 0.30f

/* ===== OCTAHEDRON / gem (radius 0.35) ===== */
static const MeshVert oct_rv[6] = {{127,0,0},{-127,0,0},{0,127,0},{0,-127,0},{0,0,127},{0,0,-127}};
static const MeshFace oct_rf[8] = {   /* CCW-from-outside winding */
    {0,2,4, 73,73,73,   MOTE_RGB565(120,220,245)},
    {0,5,2, 73,73,-73,  MOTE_RGB565(92,196,226)},
    {0,4,3, 73,-73,73,  MOTE_RGB565(106,210,236)},
    {0,3,5, 73,-73,-73, MOTE_RGB565(82,182,216)},
    {1,4,2,-73,73,73,   MOTE_RGB565(100,205,232)},
    {1,2,5,-73,73,-73,  MOTE_RGB565(86,189,221)},
    {1,3,4,-73,-73,73,  MOTE_RGB565(96,200,229)},
    {1,5,3,-73,-73,-73, MOTE_RGB565(76,176,211)},
};
static const Mesh oct_rmesh = { oct_rv, oct_rf, 6, 8, 1.0f, 1.0f, 0 };

static const Vec3 oct_hv[6] = {{0.35f,0,0},{-0.35f,0,0},{0,0.35f,0},{0,-0.35f,0},{0,0,0.35f},{0,0,-0.35f}};
static const Vec3 oct_hfn[8] = {
    {0.5774f,0.5774f,0.5774f},{0.5774f,0.5774f,-0.5774f},{0.5774f,-0.5774f,0.5774f},{0.5774f,-0.5774f,-0.5774f},
    {-0.5774f,0.5774f,0.5774f},{-0.5774f,0.5774f,-0.5774f},{-0.5774f,-0.5774f,0.5774f},{-0.5774f,-0.5774f,-0.5774f},
};
static const uint8_t oct_hfo[9] = {0,3,6,9,12,15,18,21,24};
static const uint8_t oct_hfv[24] = {0,2,4, 0,2,5, 0,3,4, 0,3,5, 1,2,4, 1,2,5, 1,3,4, 1,3,5};
static const uint8_t oct_hev[24] = {0,2,0,3,0,4,0,5, 1,2,1,3,1,4,1,5, 2,4,2,5,3,4,3,5};
static const MoteHull oct_hull = { oct_hv,6, oct_hfn,oct_hfo,oct_hfv,8, oct_hev,12, 0.35f };
#define OCT_S 0.35f

#endif
