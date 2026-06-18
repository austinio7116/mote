/*
 * A unit cube mesh in the te_mesh.h format — one colour per face so rotation
 * is obvious. CCW winding viewed from outside (engine front-face convention).
 */
#ifndef HELLO_CUBE_H
#define HELLO_CUBE_H

#include "te_mesh.h"
#include "te_config.h"

static const MeshVert k_cube_verts[8] = {
    {-127, -127, -127}, /* 0 */
    { 127, -127, -127}, /* 1 */
    { 127,  127, -127}, /* 2 */
    {-127,  127, -127}, /* 3 */
    {-127, -127,  127}, /* 4 */
    { 127, -127,  127}, /* 5 */
    { 127,  127,  127}, /* 6 */
    {-127,  127,  127}, /* 7 */
};

#define C_RED    TE_RGB565(220,  40,  40)
#define C_GREEN  TE_RGB565( 40, 200,  60)
#define C_BLUE   TE_RGB565( 60, 110, 230)
#define C_YELLOW TE_RGB565(230, 200,  40)
#define C_CYAN   TE_RGB565( 40, 200, 210)
#define C_PURPLE TE_RGB565(180,  70, 210)

static const MeshFace k_cube_faces[12] = {
    /* +Z (cyan) */
    {4, 5, 6,    0,   0, 127, C_CYAN},
    {4, 6, 7,    0,   0, 127, C_CYAN},
    /* -Z (yellow) */
    {1, 0, 3,    0,   0,-127, C_YELLOW},
    {1, 3, 2,    0,   0,-127, C_YELLOW},
    /* +X (red) */
    {5, 1, 2,  127,   0,   0, C_RED},
    {5, 2, 6,  127,   0,   0, C_RED},
    /* -X (green) */
    {0, 4, 7, -127,   0,   0, C_GREEN},
    {0, 7, 3, -127,   0,   0, C_GREEN},
    /* +Y (blue) */
    {7, 6, 2,    0, 127,   0, C_BLUE},
    {7, 2, 3,    0, 127,   0, C_BLUE},
    /* -Y (purple) */
    {0, 1, 5,    0,-127,   0, C_PURPLE},
    {0, 5, 4,    0,-127,   0, C_PURPLE},
};

static const Mesh k_cube_mesh = {
    .verts = k_cube_verts,
    .faces = k_cube_faces,
    .nverts = 8,
    .nfaces = 12,
    .scale = 1.0f,        /* half-extent 1 m */
    .bound_r = 1.8f,      /* ~sqrt(3) */
    .lod_lo = 0,
};

#endif /* HELLO_CUBE_H */
